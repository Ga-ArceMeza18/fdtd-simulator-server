#ifndef FDTD_H
#define FDTD_H

#include <stddef.h>
#include "barrier.h"
#include "status.h"
#include "snapshot_buffer.h"  

// Campo Eletromagnético

/*
* Ez: nx * ny
* Hx: nx * (ny - 1)
* Hy: (nx - 1) * ny
*
* Los campos se almacenan en memoria contigua de manera que se pueda acceder
* como un arreglo de una dimensión.
*/
struct FDTD {
    size_t nx;
    size_t ny;
    size_t steps;

    size_t sx;
    size_t sy;

    size_t num_threads;

    double *ez;
    double *hx;
    double *hy;
    
    barrier_t barrier;
    int barrier_initialized; // Indica si la barrera ha sido inicializada correctamente.

    Status *status;
    int status_error; // Indica si hubo un error al inicializar el Status.

    // Búfer de instantáneas (productor-consumidor). Cada snapshot_every pasos
    // el hilo 0 deposita una copia de Ez. snapshots == NULL: sin instantáneas.
    SnapshotBuffer *snapshots;
    unsigned long job_id;
    size_t snapshot_every;
    int snapshot_error;

    // Perfilado opcional (lo activa fdtd_bench). Cada hilo escribe solo su
    // propia posición, así que no necesita candado; se lee después del join.
    int profile;
    double *t_barrier; // Segundos que cada hilo pasó esperando en la barrera
    double t_io;       // Segundos que el hilo 0 pasó publicando Ez / depositando snapshots
};


// Inicializar los campos en cero y reservar memoria para los campos.
// Retorna 0 si la inicialización fue exitosa, -1 si hubo un error.
int fdtd_init(struct FDTD *fdtd, size_t nx, size_t ny, size_t steps,
    size_t num_threads, size_t sx, size_t sy);

// Establecer el objeto Status para la simulación FDTD.
int fdtd_set_status(struct FDTD *fdtd, Status *status);
    
int fdtd_set_snapshots(struct FDTD *fdtd, SnapshotBuffer *sb, unsigned long job_id, size_t every);

// Activa (1) o desactiva (0) la medición de tiempos por etapa.
void fdtd_set_profile(struct FDTD *fdtd, int on);

// Tiempos medidos en la última corrida (solo válidos con el perfilado activo):
// espera media y máxima en barreras entre hilos, y tiempo de publicación/E/S del hilo 0.
void fdtd_profile_times(const struct FDTD *fdtd, double *barrier_mean,
    double *barrier_max, double *io);

// Ejecutar la simulación FDTD (crea los hilos, usa la barrera reutilizable y hace join).
int fdtd_run(struct FDTD *fdtd);

// Calcular la suma de los valores de Ez.
double fdtd_sum_ez(const struct FDTD *fdtd);

// Liberar la memoria reservada para los campos.
void fdtd_destroy(struct FDTD *fdtd);

#endif // FDTD_H