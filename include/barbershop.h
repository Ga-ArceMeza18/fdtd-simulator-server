#ifndef BARBERSHOP_H
#define BARBERSHOP_H

#include <stddef.h>
#include "snapshot_buffer.h"

#define MAX_JOBS 3000 // Máximo de trabajos que el servidor acepta en total

typedef struct Barbershop Barbershop;

typedef enum {
    BARBERSHOP_OK = 0,  // Trabajo encolado
    BARBERSHOP_BUSY,    // No hay sillas libres
    BARBERSHOP_INVALID, // Parámetros inválidos
    BARBERSHOP_CLOSED,  // La barbería está cerrando 
    BARBERSHOP_FAILURE  // Error interno
} BarbershopResult;

typedef enum {
    JOB_QUEUED = 0,
    JOB_RUNNING,
    JOB_DONE,
    JOB_ERROR
} JobState;

// Parámetros de SUBMIT nx ny pasos hilos k sx sy.
typedef struct {
    size_t nx;
    size_t ny;
    size_t steps;
    size_t threads;
    size_t k;
    size_t sx;
    size_t sy;
} JobParams;

// Copia del estado de un trabajo, para responder STATUS y RESULT.
typedef struct {

    JobState state;
    size_t position;   
    size_t step;      
    size_t steps;     
    double seconds;   
    double sum_abs_ez;
    size_t last_snapshot;

} JobInfo;

// Crea la barbería con las sillas y arranca al barbero.
int barbershop_init(Barbershop **shop, size_t chairs);

// Asocia el búfer de instantáneas (NULL = sin instantáneas).
void barbershop_set_snapshots(Barbershop *shop, SnapshotBuffer *snapshots);

// SUBMIT intenta encolar un trabajo
BarbershopResult barbershop_submit(Barbershop *shop, const JobParams *params,
    unsigned long *id);

// STATUS / RESULT copia el estado del trabajo
int barbershop_query(Barbershop *shop, unsigned long id, JobInfo *info);

// PEEK lee Ez[i][j] de la última instantánea publicada
int barbershop_peek(Barbershop *shop, unsigned long id, size_t i, size_t j,
    size_t *step, double *ez);

// SHUTDOWN no acepta más trabajos, cancela los que esperan y espera a que el barbero termine el trabajo actual
void barbershop_shutdown(Barbershop *shop);

// Libera todos los recursos 
void barbershop_destroy(Barbershop *shop);

#endif // BARBERSHOP_H