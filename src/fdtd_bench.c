#define _POSIX_C_SOURCE 200809L

// fdtd_bench: ejecuta una simulación sin red e imprime una línea CSV con el
// tiempo de cada etapa.
//
//   uso: fdtd_bench [--no-header] nx ny pasos hilos k [B] [directorio]
//
// Columnas:
//   init_s          reservar mallas, Status y búfer de instantáneas (secuencial)
//   run_s           fdtd_run completo (crear hilos, calcular, join)
//   barrier_mean_s  espera media por hilo en las barreras
//   barrier_max_s   espera del hilo que más esperó
//   io_push_s       tiempo del hilo 0 publicando Ez y depositando snapshots
//   compute_s       run_s - barrier_mean_s - io_push_s (cómputo "puro" estimado)
//   flush_s         espera posterior a que el hilo escritor termine de guardar a disco
//   total_s         init_s + run_s + flush_s
//   sum_abs_ez      suma de |Ez| (para comparar con RESULT del servidor)
//   hash_ez         FNV-1a de los bytes de Ez: sirve para comprobar el determinismo
//
// La fuente se coloca en el centro (nx/2, ny/2). Los snapshots (k > 0) se
// borran al terminar; B es la capacidad del búfer (por defecto 4).

#include "fdtd_solver.h"
#include "snapshot_buffer.h"
#include "status.h"

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static double ahora(void) {
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static int parse_size(const char *text, size_t *out) {
    char *end;
    unsigned long long value;

    if (text == NULL || !isdigit((unsigned char)text[0])) {
        return -1;
    }
    errno = 0;
    value = strtoull(text, &end, 10);
    if (errno != 0 || *end != '\0' || value > SIZE_MAX) {
        return -1;
    }
    *out = (size_t)value;
    return 0;
}

static uint64_t hash_bytes(const void *data, size_t size) {
    const unsigned char *p = data;
    uint64_t h = 14695981039346656037ULL;
    size_t i;

    for (i = 0; i < size; i++) {
        h ^= p[i];
        h *= 1099511628211ULL;
    }
    return h;
}

int main(int argc, char *argv[]) {
    int header = 1;
    size_t v[5];
    size_t buffer = 4;
    char dir_default[64];
    const char *dir;
    struct FDTD fdtd;
    Status *status = NULL;
    SnapshotBuffer *snapshots = NULL;
    double t0, t1, t2, t3;
    double init_s, run_s, flush_s;
    double barrier_mean = 0.0, barrier_max = 0.0, io = 0.0, compute_s;
    size_t nx, ny, steps, threads, k, q;
    int rc;
    int i;

    if (argc > 1 && strcmp(argv[1], "--no-header") == 0) {
        header = 0;
        argc--;
        argv++;
    }
    if (argc < 6 || argc > 8) {
        fprintf(stderr, "uso: %s [--no-header] nx ny pasos hilos k [B] [directorio]\n",
            argv[0]);
        return EXIT_FAILURE;
    }
    for (i = 0; i < 5; i++) {
        if (parse_size(argv[i + 1], &v[i]) != 0) {
            fprintf(stderr, "argumento no numerico: %s\n", argv[i + 1]);
            return EXIT_FAILURE;
        }
    }
    nx = v[0]; ny = v[1]; steps = v[2]; threads = v[3]; k = v[4];

    if (argc >= 7 && (parse_size(argv[6], &buffer) != 0 || buffer == 0)) {
        fprintf(stderr, "B debe ser un entero positivo\n");
        return EXIT_FAILURE;
    }
    snprintf(dir_default, sizeof(dir_default), "/tmp/fdtd_bench_%ld", (long)getpid());
    dir = (argc == 8) ? argv[7] : dir_default;

    if (nx < 3 || ny < 3 || steps == 0 || threads < 1 || threads > nx - 2
        || k > steps) {
        fprintf(stderr, "parametros invalidos (nx,ny>=3; pasos>0; 1<=hilos<=nx-2; 0<=k<=pasos)\n");
        return EXIT_FAILURE;
    }

    // ---- inicialización
    t0 = ahora();
    if (fdtd_init(&fdtd, nx, ny, steps, threads, nx / 2, ny / 2) != 0) {
        fprintf(stderr, "fdtd_init fallo\n");
        return EXIT_FAILURE;
    }
    if (status_init(&status, nx, ny) != 0) {
        fprintf(stderr, "status_init fallo\n");
        fdtd_destroy(&fdtd);
        return EXIT_FAILURE;
    }
    fdtd_set_status(&fdtd, status);
    if (k > 0) {
        if (snapbuf_init(&snapshots, buffer, dir) != 0) {
            fprintf(stderr, "snapbuf_init fallo en '%s'\n", dir);
            status_destroy(status);
            fdtd_destroy(&fdtd);
            return EXIT_FAILURE;
        }
        fdtd_set_snapshots(&fdtd, snapshots, 1, k);
    }
    fdtd_set_profile(&fdtd, 1);
    t1 = ahora();

    // ---- cómputo
    rc = fdtd_run(&fdtd);
    t2 = ahora();

    // ---- espera al hilo escritor
    if (snapshots != NULL) {
        snapbuf_flush(snapshots);
        if (snapbuf_errors(snapshots) > 0) {
            rc = -1;
        }
    }
    t3 = ahora();

    init_s = t1 - t0;
    run_s = t2 - t1;
    flush_s = t3 - t2;
    fdtd_profile_times(&fdtd, &barrier_mean, &barrier_max, &io);
    
    compute_s = run_s - barrier_mean - io;
    if (compute_s < 0.0) {
        compute_s = 0.0;
    }

    if (rc == 0) {
        if (header) {
            printf("nx,ny,steps,threads,k,B,init_s,run_s,barrier_mean_s,"
                "barrier_max_s,io_push_s,compute_s,flush_s,total_s,"
                "sum_abs_ez,hash_ez\n");
        }
        printf("%zu,%zu,%zu,%zu,%zu,%zu,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.10e,%016" PRIx64 "\n",
            nx, ny, steps, threads, k, (k > 0) ? buffer : (size_t)0,
            init_s, run_s, barrier_mean, barrier_max, io, compute_s, flush_s,
            init_s + run_s + flush_s, fdtd_sum_ez(&fdtd),
            hash_bytes(fdtd.ez, nx * ny * sizeof(double)));
    } else {
        fprintf(stderr, "la simulacion fallo\n");
    }

    // ---- limpieza: se borran los snapshots generados
    if (snapshots != NULL) {
        char path[512];

        for (q = k; q <= steps; q += k) {
            if (snapbuf_path(snapshots, 1, q, path, sizeof(path)) == 0) {
                remove(path);
            }
        }
        snapbuf_destroy(snapshots);
        rmdir(dir);
    }
    status_destroy(status);
    fdtd_destroy(&fdtd);

    return (rc == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}