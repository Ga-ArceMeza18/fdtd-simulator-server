#ifndef SNAPSHOT_BUFFER_H
#define SNAPSHOT_BUFFER_H

#include <stddef.h>

// Búfer acotado de instantáneas (productor-consumidor).
//
// Productor: el hilo 0 del solucionador, cada K pasos, copia Ez y la deposita.
// Consumidor: el hilo escritor, que guarda cada instantánea a disco en el
// archivo job<id>_step<q>.bin con tres enteros de 32 bits (nx, ny, q)
// seguidos de nx*ny valores double en orden de filas.
typedef struct SnapshotBuffer SnapshotBuffer;

int snapbuf_init(SnapshotBuffer **sb, size_t capacity, const char *dir);

int snapbuf_push(SnapshotBuffer *sb, unsigned long job_id, size_t nx, size_t ny,
    size_t step, const double *ez);

void snapbuf_flush(SnapshotBuffer *sb);

size_t snapbuf_errors(SnapshotBuffer *sb);

int snapbuf_path(const SnapshotBuffer *sb, unsigned long job_id, size_t step,
    char *out, size_t size);

void snapbuf_destroy(SnapshotBuffer *sb);

#endif // SNAPSHOT_BUFFER_H