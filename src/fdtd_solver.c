#include "fdtd_solver.h"

#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define PI 3.14159265358979323846
#define SC 0.70710678118654752440
#define IMPEDANCIA_VACÍO 376.730313668
#define EPSILON_R 1.0
#define PPW 20.0

// Información que recibe cada hilo solucionador.
struct ThreadData {
    struct FDTD *fdtd;
    struct StartGate *start_gate;
    size_t tid;
};

// Evita que los hilos comiencen el cálculo hasta saber que todos fueron creados

struct StartGate {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    int iniciar;
    int cancelar;
};

// Funciones index_ para calcular el índice de un elemento en los arreglos de campos Ez, Hx y Hy.
static size_t index_ez(const struct FDTD *fdtd, size_t m, size_t n) {
    return m * fdtd->ny + n;
}

static size_t index_hx(const struct FDTD *fdtd, size_t m, size_t n) {
    return m * (fdtd->ny - 1) + n;
}

static size_t index_hy(const struct FDTD *fdtd, size_t m, size_t n) {
    return m * fdtd->ny + n;
}

// Reparte el intervalo [inicio, fin] entre los hilos.
static void repartir_filas(size_t inicio, size_t fin, size_t tid, 
            size_t cantidad_hilos,
            size_t *primera,
            size_t *ultima) {

    size_t total;
    size_t base;
    size_t resto;

    total = fin - inicio;
    base = total / cantidad_hilos;
    resto = total % cantidad_hilos;

    *primera = inicio + tid * base;

    if (tid < resto) {
        *primera += tid;
        *ultima = *primera + base + 1;
    } else {
        *primera += resto;
        *ultima = *primera + base;
    }
}

// Función de fuente suave de Ricker.
static double ricker(size_t q) {
    double arg;
    double cuadrado;

    arg = PI * ((SC * (double)q / PPW) - 1.0);
    cuadrado = arg * arg;

    return (1.0 - 2.0 * cuadrado) * exp(-cuadrado);
}

// Fase A1: actualización de Hx.
static void actualizar_hx(struct FDTD *fdtd, size_t inicio, size_t fin) {
    size_t m;
    size_t n;
    double factor;

    factor = SC / IMPEDANCIA_VACÍO;

    for (m = inicio; m < fin; m++) {
        for (n = 0; n < fdtd->ny - 1; n++) {
            size_t h;
            size_t e0;
            size_t e1;

            h = index_hx(fdtd, m, n);
            e0 = index_ez(fdtd, m, n);
            e1 = index_ez(fdtd, m, n + 1);

            fdtd->hx[h] -= factor * (fdtd->ez[e1] - fdtd->ez[e0]);
        }
    }
}

// Fase A2: actualización de Hy.
static void actualizar_hy(struct FDTD *fdtd, size_t inicio, size_t fin) {
    size_t m;
    size_t n;
    double factor;

    factor = SC / IMPEDANCIA_VACÍO;

    for (m = inicio; m < fin; m++) {
        for (n = 0; n < fdtd->ny; n++) {
            size_t h;
            size_t e0;
            size_t e1;

            h = index_hy(fdtd, m, n);
            e0 = index_ez(fdtd, m, n);
            e1 = index_ez(fdtd, m + 1, n);

            fdtd->hy[h] += factor * (fdtd->ez[e1] - fdtd->ez[e0]);
        }
    }
}

// Fase B: actualización de Ez interior.
static void actualizar_ez(struct FDTD *fdtd, size_t inicio, size_t fin) {
    size_t m;
    size_t n;
    double factor;

    factor = SC * IMPEDANCIA_VACÍO / EPSILON_R;

    for (m = inicio; m < fin; m++) {
        for (n = 1; n < fdtd->ny - 1; n++) {
            size_t e;
            size_t hy_actual;
            size_t hy_anterior;
            size_t hx_actual;
            size_t hx_anterior;

            e = index_ez(fdtd, m, n);

            hy_actual = index_hy(fdtd, m, n);
            hy_anterior = index_hy(fdtd, m - 1, n);

            hx_actual = index_hx(fdtd, m, n);
            hx_anterior = index_hx(fdtd, m, n - 1);

            fdtd->ez[e] += factor * ((fdtd->hy[hy_actual]
                - fdtd->hy[hy_anterior]) - (fdtd->hx[hx_actual]
                - fdtd->hx[hx_anterior]));
        }
    }
}

// Un hilo ejecuta este procedimiento por cada paso temporal.

static void *ejecutar_hilo(void *arg) {
    struct ThreadData *datos;
    struct FDTD *fdtd;
    size_t tid;
    size_t q;
    size_t inicio;
    size_t fin;

    datos = (struct ThreadData *)arg;
    fdtd = datos->fdtd;
    tid = datos->tid;

    /// Espera hasta que fdtd_run confirme que todos los hilos se crearon correctamente.
    pthread_mutex_lock(&datos->start_gate->mutex);
    while (!datos->start_gate->iniciar &&
           !datos->start_gate->cancelar) {
        pthread_cond_wait(&datos->start_gate->cond,
                          &datos->start_gate->mutex);
    }

    if (datos->start_gate->cancelar) {
        pthread_mutex_unlock(&datos->start_gate->mutex);
        return NULL;
    }

    pthread_mutex_unlock(&datos->start_gate->mutex);

    for (q = 0; q < fdtd->steps; q++) {
        // FASE A: campos magnéticos.
        // Cada campo tiene sus propios límites de filas.

        repartir_filas(0, fdtd->nx, tid, fdtd->num_threads, &inicio, &fin);
        actualizar_hx(fdtd, inicio, fin);

        repartir_filas(0, fdtd->nx - 1, tid, fdtd->num_threads, &inicio, &fin);
        actualizar_hy(fdtd, inicio, fin);

        // PENDIENTE 1: BARRERA REUTILIZABLE.
        // Debe ir aquí, después de actualizar Hx y Hy, antes de comenzar la fase B.
        barrier_wait(&fdtd->barrier);

        // FASE B: campo eléctrico interior.
        repartir_filas(1, fdtd->nx - 1, tid, fdtd->num_threads, &inicio, &fin);
        actualizar_ez(fdtd, inicio, fin);

        // Fuente Ricker: solo la aplica el hilo propietario de la fila sx, exactamente una vez por paso.
        if (fdtd->sx >= inicio && fdtd->sx < fin) {
            size_t fuente;
            fuente = index_ez(fdtd, fdtd->sx, fdtd->sy);
            fdtd->ez[fuente] += ricker(q);
        }

        // PENDIENTE 2: BARRERA REUTILIZABLE.
        // Debe ir aquí, después de actualizar Ez y aplicar la fuente, antes de comenzar el siguiente paso temporal.
        barrier_wait(&fdtd->barrier);
    }

    return NULL;
}

int fdtd_init(struct FDTD *fdtd, size_t nx, size_t ny, size_t steps,
            size_t num_threads, size_t sx, size_t sy) {

    size_t cantidad_ez;
    size_t cantidad_hx;
    size_t cantidad_hy;

    if (fdtd == NULL) {
        return -1;
    }

    memset(fdtd, 0, sizeof(*fdtd));

    if (nx < 3 || ny < 3 || steps == 0 || num_threads == 0) {
        return -1;
    }

    if (num_threads > nx - 2) {
        return -1;
    }

    if (sx == 0 || sx >= nx - 1 || sy == 0 || sy >= ny - 1) {
        return -1;
    }

    // Evitar desbordamiento al calcular los tamaños de los arreglos.
    if (nx > SIZE_MAX / ny || nx > SIZE_MAX / (ny - 1)
        || nx - 1 > SIZE_MAX / ny) {

        return -1;
    }

    cantidad_ez = nx * ny;
    cantidad_hx = nx * (ny - 1);
    cantidad_hy = (nx - 1) * ny;

    // Evitar desbordamiento al calcular los tamaños de los arreglos.
    if (cantidad_ez > SIZE_MAX / sizeof(double) ||
        cantidad_hx > SIZE_MAX / sizeof(double) ||
        cantidad_hy > SIZE_MAX / sizeof(double)) {
        return -1;
    }

    fdtd->ez = calloc(cantidad_ez, sizeof(double));
    fdtd->hx = calloc(cantidad_hx, sizeof(double));
    fdtd->hy = calloc(cantidad_hy, sizeof(double));

    if (fdtd->ez == NULL || fdtd->hx == NULL || fdtd->hy == NULL) {
        fdtd_destroy(fdtd);
        return -1;
    }

    fdtd->nx = nx;
    fdtd->ny = ny;
    fdtd->steps = steps;
    fdtd->num_threads = num_threads;
    fdtd->sx = sx;
    fdtd->sy = sy;

    // Inicializar la barrera reutilizable
    if (barrier_init(&fdtd->barrier, num_threads) != 0) {
        fdtd_destroy(fdtd);
        return -1;
    }

    return 0;
}

// Ejecutar la simulación FDTD.
// En general lo que hace es crear los hilos, esperar a que todos estén listos y luego iniciar la simulación. 

int fdtd_run(struct FDTD *fdtd) {

    pthread_t *threads;
    struct ThreadData *datos;
    size_t creados;
    size_t i;
    int error;
    
    struct StartGate start_gate;
    datos[i].fdtd = fdtd;
    datos[i].start_gate = &start_gate;
    datos[i].tid = i;

    if (fdtd == NULL || fdtd->ez == NULL || fdtd->hx == NULL ||
        fdtd->hy == NULL) {
        return -1;
    }

    if (fdtd->num_threads > SIZE_MAX / sizeof(pthread_t) ||
        fdtd->num_threads > SIZE_MAX / sizeof(struct ThreadData)) {
        return -1;
    }

    threads = malloc(fdtd->num_threads * sizeof(pthread_t));
    datos = malloc(fdtd->num_threads * sizeof(struct ThreadData));

    if (threads == NULL || datos == NULL) {
        free(threads);
        free(datos);
        return -1;
    }

    //La compuerta de inicio evita el bloqueo si falla la creación parcial de los hilos.
    if (pthread_mutex_init(&start_gate.mutex, NULL) != 0) {
        free(threads);
        free(datos);
        return -1;
    }

    if (pthread_cond_init(&start_gate.cond, NULL) != 0) {
        pthread_mutex_destroy(&start_gate.mutex);
        free(threads);
        free(datos);
        return -1;
    }

    start_gate.iniciar = 0;
    start_gate.cancelar = 0;

    creados = 0;
    error = 0;

    for (i = 0; i < fdtd->num_threads; i++) {
        datos[i].fdtd = fdtd;
        datos[i].tid = i;

        if (pthread_create(&threads[i], NULL, ejecutar_hilo, &datos[i]) != 0) {
            error = -1;
            break;
        }

        creados++;
    }

    pthread_mutex_lock(&start_gate.mutex);

    if (error == 0) {
        start_gate.iniciar = 1;
    } else {
        start_gate.cancelar = 1;
    }

    pthread_cond_broadcast(&start_gate.cond);
    pthread_mutex_unlock(&start_gate.mutex);

    for (i = 0; i < creados; i++) {
        if (pthread_join(threads[i], NULL) != 0) {
            error = -1;
        }
    }

    pthread_cond_destroy(&start_gate.cond);
    pthread_mutex_destroy(&start_gate.mutex);

    free(threads);
    free(datos);

    return error;
}

double fdtd_sum_ez(const struct FDTD *fdtd) {
    size_t i;
    size_t total;
    double suma;

    if (fdtd == NULL || fdtd->ez == NULL) {
        return 0.0;
    }

    total = fdtd->nx * fdtd->ny;
    suma = 0.0;

    for (i = 0; i < total; i++) {
        suma += fabs(fdtd->ez[i]);
    }

    return suma;
}

void fdtd_destroy(struct FDTD *fdtd) {
    if (fdtd == NULL) {
        return;
    }

    // Destruir la barrera reutilizable
    barrier_destroy(&fdtd->barrier);

    free(fdtd->ez);
    free(fdtd->hx);
    free(fdtd->hy);

    memset(fdtd, 0, sizeof(*fdtd));
}
