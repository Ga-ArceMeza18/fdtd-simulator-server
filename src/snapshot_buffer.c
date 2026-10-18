#define _POSIX_C_SOURCE 200809L

#include "snapshot_buffer.h"

#include <errno.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#define PATH_MAX_LEN 512

// Una posición del búfer. data == NULL es la "píldora envenenada" que le
// indica al escritor que ya no vendrán más instantáneas.
typedef struct {
    unsigned long job_id;
    size_t nx;
    size_t ny;
    size_t step;
    double *data;
} Slot;

struct SnapshotBuffer {
    Slot *slots;
    size_t capacity;
    size_t in;   // Donde el productor deposita la siguiente instantánea
    size_t out;  // De donde el escritor toma la siguiente

    sem_t empty; // Espacios libres (inicia en B)
    sem_t full;  // Instantáneas listas para escribir (inicia en 0)

    // Un solo mutex protege los índices y los contadores de abajo.
    pthread_mutex_t mutex;
    pthread_cond_t drained; // Se avisa cada vez que el escritor termina una
    size_t pushed;  // Instantáneas depositadas (sin contar la píldora)
    size_t written; // Instantáneas ya procesadas por el escritor
    size_t errors;  // Instantáneas que no se pudieron guardar

    char *dir;
    pthread_t writer;
};

// Reintenta sem_wait si una señal interrumpe la llamada.
static void wait_sem(sem_t *sem) {
    while (sem_wait(sem) == -1 && errno == EINTR) {
    }
}

// Deposita un Slot ya armado (se usa para la píldora).
static void enqueue(SnapshotBuffer *sb, Slot slot, int count_it) {
    wait_sem(&sb->empty);

    pthread_mutex_lock(&sb->mutex);
    sb->slots[sb->in] = slot;
    sb->in = (sb->in + 1) % sb->capacity;
    if (count_it) {
        sb->pushed++;
    }
    pthread_mutex_unlock(&sb->mutex);

    sem_post(&sb->full);
}

int snapbuf_path(const SnapshotBuffer *sb, unsigned long job_id, size_t step,
    char *out, size_t size) {

    int n;

    if (sb == NULL || out == NULL) {
        return -1;
    }

    if (strcmp(sb->dir, ".") == 0) {
        n = snprintf(out, size, "job%lu_step%zu.bin", job_id, step);
    } else {
        n = snprintf(out, size, "%s/job%lu_step%zu.bin", sb->dir, job_id, step);
    }

    return (n < 0 || (size_t)n >= size) ? -1 : 0;
}

// Se escribe a un archivo temporal y luego se renombra, para que nadie
// vea nunca un .bin a medias.
static int write_snapshot(const SnapshotBuffer *sb, const Slot *slot) {
    char path[PATH_MAX_LEN];
    char tmp[PATH_MAX_LEN + 8];
    int32_t header[3];
    FILE *file;
    size_t count;
    int ok;

    if (snapbuf_path(sb, slot->job_id, slot->step, path, sizeof(path)) != 0) {
        return -1;
    }
    if (snprintf(tmp, sizeof(tmp), "%s.tmp", path) >= (int)sizeof(tmp)) {
        return -1;
    }

    header[0] = (int32_t)slot->nx;
    header[1] = (int32_t)slot->ny;
    header[2] = (int32_t)slot->step;
    count = slot->nx * slot->ny;

    file = fopen(tmp, "wb");
    if (file == NULL) {
        return -1;
    }

    ok = fwrite(header, sizeof(header[0]), 3, file) == 3
        && fwrite(slot->data, sizeof(double), count, file) == count;

    if (fclose(file) != 0) {
        ok = 0;
    }
    if (!ok) {
        remove(tmp);
        return -1;
    }

    if (rename(tmp, path) != 0) {
        remove(tmp);
        return -1;
    }

    return 0;
}

// Hilo escritor: el consumidor del búfer.
static void *writer_thread(void *arg) {
    SnapshotBuffer *sb = arg;

    while (1) {
        Slot slot;
        int failed;

        wait_sem(&sb->full); // Duerme mientras no haya instantáneas

        pthread_mutex_lock(&sb->mutex);
        slot = sb->slots[sb->out];
        sb->out = (sb->out + 1) % sb->capacity;
        pthread_mutex_unlock(&sb->mutex);

        sem_post(&sb->empty); // Libera el espacio antes de escribir a disco

        if (slot.data == NULL) { // Píldora: ya se vació todo lo anterior
            break;
        }

        failed = write_snapshot(sb, &slot) != 0;
        free(slot.data);

        pthread_mutex_lock(&sb->mutex);
        if (failed) {
            sb->errors++;
            fprintf(stderr, "snapshot: no se pudo guardar job%lu paso %zu\n",
                slot.job_id, slot.step);
        }
        sb->written++;
        pthread_cond_broadcast(&sb->drained);
        pthread_mutex_unlock(&sb->mutex);
    }

    return NULL;
}

int snapbuf_init(SnapshotBuffer **sb, size_t capacity, const char *dir) {
    SnapshotBuffer *s;

    if (sb == NULL || capacity == 0 || capacity > (size_t)INT32_MAX) {
        return -1;
    }
    if (dir == NULL) {
        dir = ".";
    }

    s = calloc(1, sizeof(*s));
    if (s == NULL) {
        return -1;
    }

    s->capacity = capacity;
    s->slots = calloc(capacity, sizeof(Slot));
    s->dir = strdup(dir);
    if (s->slots == NULL || s->dir == NULL) {
        goto fail_alloc;
    }

    // Crea el directorio si no existe
    if (mkdir(dir, 0777) != 0 && errno != EEXIST) {
        goto fail_alloc;
    }

    if (sem_init(&s->empty, 0, (unsigned int)capacity) != 0) {
        goto fail_alloc;
    }
    if (sem_init(&s->full, 0, 0) != 0) {
        goto fail_empty;
    }
    if (pthread_mutex_init(&s->mutex, NULL) != 0) {
        goto fail_full;
    }
    if (pthread_cond_init(&s->drained, NULL) != 0) {
        goto fail_mutex;
    }
    if (pthread_create(&s->writer, NULL, writer_thread, s) != 0) {
        goto fail_cond;
    }

    *sb = s;
    return 0;

fail_cond:
    pthread_cond_destroy(&s->drained);
fail_mutex:
    pthread_mutex_destroy(&s->mutex);
fail_full:
    sem_destroy(&s->full);
fail_empty:
    sem_destroy(&s->empty);
fail_alloc:
    free(s->slots);
    free(s->dir);
    free(s);
    return -1;
}

int snapbuf_push(SnapshotBuffer *sb, unsigned long job_id, size_t nx, size_t ny,
    size_t step, const double *ez) {

    Slot slot;
    size_t count;

    if (sb == NULL || ez == NULL || nx == 0 || ny == 0) {
        return -1;
    }
    // El formato guarda (nx, ny, q) como enteros de 32 bits
    if (nx > (size_t)INT32_MAX || ny > (size_t)INT32_MAX
        || step > (size_t)INT32_MAX) {
        return -1;
    }
    if (nx > SIZE_MAX / ny || nx * ny > SIZE_MAX / sizeof(double)) {
        return -1;
    }
    count = nx * ny;

    // Primero se reserva el espacio y recién después se copia: así nunca hay
    // más de B copias vivas en el búfer, que es justo la cota de memoria.
    wait_sem(&sb->empty);

    slot.data = malloc(count * sizeof(double));
    if (slot.data == NULL) {
        sem_post(&sb->empty); // Devuelve el espacio reservado
        return -1;
    }
    memcpy(slot.data, ez, count * sizeof(double)); // Copia fuera del mutex
    slot.job_id = job_id;
    slot.nx = nx;
    slot.ny = ny;
    slot.step = step;

    pthread_mutex_lock(&sb->mutex);
    sb->slots[sb->in] = slot;
    sb->in = (sb->in + 1) % sb->capacity;
    sb->pushed++;
    pthread_mutex_unlock(&sb->mutex);

    sem_post(&sb->full);
    return 0;
}

void snapbuf_flush(SnapshotBuffer *sb) {
    if (sb == NULL) {
        return;
    }

    pthread_mutex_lock(&sb->mutex);
    while (sb->written < sb->pushed) {
        pthread_cond_wait(&sb->drained, &sb->mutex);
    }
    pthread_mutex_unlock(&sb->mutex);
}

size_t snapbuf_errors(SnapshotBuffer *sb) {
    size_t errors;

    if (sb == NULL) {
        return 0;
    }

    pthread_mutex_lock(&sb->mutex);
    errors = sb->errors;
    pthread_mutex_unlock(&sb->mutex);

    return errors;
}

void snapbuf_destroy(SnapshotBuffer *sb) {
    Slot pill;

    if (sb == NULL) {
        return;
    }

    // La píldora entra por el final de la cola FIFO: el escritor primero
    // guarda todo lo que ya estaba depositado y luego termina.
    memset(&pill, 0, sizeof(pill));
    enqueue(sb, pill, 0);
    pthread_join(sb->writer, NULL);

    pthread_cond_destroy(&sb->drained);
    pthread_mutex_destroy(&sb->mutex);
    sem_destroy(&sb->full);
    sem_destroy(&sb->empty);

    free(sb->slots);
    free(sb->dir);
    free(sb);
}