#define _POSIX_C_SOURCE 200809L

// Búfer acotado de instantáneas (productor-consumidor) y escritor a disco.

#include "snapshot_buffer.h"
#include "test_util.h"

#include <pthread.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/types.h>

static char dir[64];

// Verifica el formato (nx, ny, q como int32 + nx*ny doubles) y borra el archivo.
static int check_file(unsigned long job, size_t step, size_t nx, size_t ny, double value) {
    char path[256];
    char tmp[300];
    int32_t h[3];
    FILE *f;
    double *d;
    size_t i;
    int ok = 1;

    snprintf(path, sizeof(path), "%s/job%lu_step%zu.bin", dir, job, step);
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);

    f = fopen(path, "rb");
    if (f == NULL) {
        return 0;
    }
    d = malloc(nx * ny * sizeof(double));
    if (fread(h, sizeof(h[0]), 3, f) != 3 || h[0] != (int32_t)nx
        || h[1] != (int32_t)ny || h[2] != (int32_t)step
        || fread(d, sizeof(double), nx * ny, f) != nx * ny) {
        ok = 0;
    }
    for (i = 0; ok && i < nx * ny; i++) {
        if (d[i] != value) {
            ok = 0;
        }
    }
    fclose(f);
    free(d);
    remove(path);
    if (access(tmp, F_OK) == 0) { // No debe quedar ningún .tmp
        ok = 0;
    }
    return ok;
}

static double *make_grid(size_t nx, size_t ny, double value) {
    double *g = malloc(nx * ny * sizeof(double));
    size_t i;

    for (i = 0; i < nx * ny; i++) {
        g[i] = value;
    }
    return g;
}

typedef struct {
    SnapshotBuffer *sb;
    unsigned long job;
    size_t count;
    int errors;
} Producer;

static void *producer(void *arg) {
    Producer *p = arg;
    size_t s;

    for (s = 1; s <= p->count; s++) {
        double *g = make_grid(20, 30, (double)(p->job * 1000 + s));

        if (snapbuf_push(p->sb, p->job, 20, 30, s, g) != 0) {
            p->errors++;
        }
        free(g);
    }
    return NULL;
}

int main(void) {
    SnapshotBuffer *sb = NULL;
    double *g;
    size_t s;
    int i;
    char path[128];

    TEST_START("snapshot_buffer");

    snprintf(dir, sizeof(dir), "/tmp/fdtd_snap_test_XXXXXX");
    CHECK(mkdtemp(dir) != NULL);

    // Argumentos inválidos
    CHECK(snapbuf_init(NULL, 4, dir) == -1);
    CHECK(snapbuf_init(&sb, 0, dir) == -1);
    g = make_grid(2, 2, 1.0);
    CHECK(snapbuf_push(NULL, 1, 2, 2, 1, g) == -1);

    // Nombre del archivo
    CHECK(snapbuf_init(&sb, 2, dir) == 0);
    CHECK(snapbuf_push(sb, 1, 0, 2, 1, g) == -1);
    CHECK(snapbuf_push(sb, 1, 2, 2, 1, NULL) == -1);
    CHECK(snapbuf_path(sb, 7, 30, path, sizeof(path)) == 0);
    {
        char esperado[128];
        snprintf(esperado, sizeof(esperado), "%s/job7_step30.bin", dir);
        CHECK(strcmp(path, esperado) == 0);
    }
    CHECK(snapbuf_path(sb, 7, 30, path, 5) == -1); // No cabe
    free(g);

    // 1) Un productor, capacidad B = 2 (mucho más pequeña que las 30 instantáneas)
    {
        Producer p = {sb, 1, 30, 0};

        producer(&p);
        CHECK(p.errors == 0);
        snapbuf_flush(sb);
        CHECK(snapbuf_errors(sb) == 0);
        for (s = 1; s <= 30; s++) {
            CHECK(check_file(1, s, 20, 30, (double)(1000 + s)));
        }
    }

    // 2) Varios productores a la vez
    {
        pthread_t th[4];
        Producer p[4];

        for (i = 0; i < 4; i++) {
            p[i].sb = sb;
            p[i].job = (unsigned long)(2 + i);
            p[i].count = 25;
            p[i].errors = 0;
            pthread_create(&th[i], NULL, producer, &p[i]);
        }
        for (i = 0; i < 4; i++) {
            pthread_join(th[i], NULL);
            CHECK(p[i].errors == 0);
        }
        snapbuf_flush(sb);
        CHECK(snapbuf_errors(sb) == 0);
        for (i = 0; i < 4; i++) {
            for (s = 1; s <= 25; s++) {
                CHECK(check_file((unsigned long)(2 + i), s, 20, 30,
                    (double)((2 + i) * 1000 + s)));
            }
        }
    }
    snapbuf_destroy(sb);

    // 3) destroy sin flush previo: la píldora entra al final de la cola FIFO,
    //    así que todo lo depositado se guarda antes de terminar (B = 1).
    CHECK(snapbuf_init(&sb, 1, dir) == 0);
    {
        Producer p = {sb, 9, 10, 0};

        producer(&p);
        CHECK(p.errors == 0);
    }
    snapbuf_destroy(sb);
    for (s = 1; s <= 10; s++) {
        CHECK(check_file(9, s, 20, 30, (double)(9000 + s)));
    }

    // 4) Fallos de disco: se cuentan, no se pierde la cuenta ni se cuelga flush
    CHECK(snapbuf_init(&sb, 2, dir) == 0);
    CHECK(rmdir(dir) == 0); // El destino desaparece
    {
        Producer p = {sb, 5, 3, 0};

        producer(&p);
        CHECK(p.errors == 0); // El push no falla; falla el escritor
        snapbuf_flush(sb);
        CHECK(snapbuf_errors(sb) == 3);
    }
    snapbuf_destroy(sb);

    TEST_END();
}