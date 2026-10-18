#define _POSIX_C_SOURCE 200809L

#include "barbershop.h"
#include "fdtd_solver.h"
#include "status.h"

#include <pthread.h>
#include <semaphore.h>
#include <stdlib.h>
#include <time.h>

// Un trabajo de simulación 
typedef struct {
    JobParams params;
    JobState state;
    double seconds;
    double sum_abs_ez;
    Status *status;
    unsigned long id;
} Job;

struct Barbershop {
    Job **chairs_queue; // Sillas de espera 
    size_t chairs;      // Número de sillas
    size_t head;        // Primer cliente de la fila
    size_t waiting;     // Clientes sentados esperando

    Job *jobs[MAX_JOBS]; // Todos los trabajos aceptados. El id k está en jobs[k - 1].
    size_t total_jobs;

    pthread_mutex_t mutex; 
    sem_t customers;       // Clientes esperando
    pthread_t barber;
    int closing;           // 1 cuando se recibió SHUTDOWN.

    SnapshotBuffer *snapshots; 
};

// Revisa los parámetros con las mismas reglas que fdtd_init
static int valid_params(const JobParams *p) {
    return p->nx >= 3 && p->ny >= 3 && p->steps > 0
        && p->threads >= 1 && p->threads <= p->nx - 2
        && p->sx >= 1 && p->sx <= p->nx - 2
        && p->sy >= 1 && p->sy <= p->ny - 2
        && p->k <= p->steps;
}

// Ejecuta la simulación: fdtd_run crea los T hilos y les hace join.
// El Status (copia de Ez publicada) se reserva aquí y no en SUBMIT, así los
// trabajos en cola no consumen nx*ny doubles mientras esperan.
static void run_job(Barbershop *shop, Job *job) {
    struct FDTD fdtd;
    struct timespec start;
    struct timespec end;
    const JobParams *p = &job->params;
    SnapshotBuffer *snapshots;
    Status *status = NULL;
    size_t errors_before = 0;
    int error = 1;

    pthread_mutex_lock(&shop->mutex);
    snapshots = shop->snapshots;
    pthread_mutex_unlock(&shop->mutex);

    if (status_init(&status, p->nx, p->ny) == 0) {
        pthread_mutex_lock(&shop->mutex);
        job->status = status;
        pthread_mutex_unlock(&shop->mutex);

        if (fdtd_init(&fdtd, p->nx, p->ny, p->steps, p->threads, p->sx, p->sy) == 0) {
            fdtd_set_status(&fdtd, status);

            if (snapshots != NULL && p->k > 0) {
                fdtd_set_snapshots(&fdtd, snapshots, job->id, p->k);
                errors_before = snapbuf_errors(snapshots);
            }

            clock_gettime(CLOCK_MONOTONIC, &start);
            error = fdtd_run(&fdtd);

            if (snapshots != NULL && p->k > 0) {
                snapbuf_flush(snapshots);
                // Solo cuentan los errores ocurridos durante ESTE trabajo.
                if (snapbuf_errors(snapshots) > errors_before) {
                    error = 1;
                }
            }

            clock_gettime(CLOCK_MONOTONIC, &end);

            pthread_mutex_lock(&shop->mutex);
            job->seconds = (double)(end.tv_sec - start.tv_sec)
                + (double)(end.tv_nsec - start.tv_nsec) / 1e9;
            job->sum_abs_ez = fdtd_sum_ez(&fdtd);
            pthread_mutex_unlock(&shop->mutex);

            fdtd_destroy(&fdtd);
        }
    }

    pthread_mutex_lock(&shop->mutex);
    job->state = (error == 0) ? JOB_DONE : JOB_ERROR;
    pthread_mutex_unlock(&shop->mutex);
}

// Hilo del barbero (despachador)
static void *barber_thread(void *arg) {
    Barbershop *shop = arg;
    Job *job;

    while (1) {
        sem_wait(&shop->customers); // Espera a que haya un cliente si es 0 se duerme.

        pthread_mutex_lock(&shop->mutex);
        if (shop->closing) { 
            pthread_mutex_unlock(&shop->mutex);
            break;
        }

        // El cliente deja la silla 
        job = shop->chairs_queue[shop->head];
        shop->head = (shop->head + 1) % shop->chairs;
        shop->waiting--;
        job->state = JOB_RUNNING;
        pthread_mutex_unlock(&shop->mutex);

        run_job(shop, job);
    }

    return NULL;
}

int barbershop_init(Barbershop **shop, size_t chairs) {
    Barbershop *s;

    if (shop == NULL || chairs == 0) {
        return -1;
    }

    s = calloc(1, sizeof(Barbershop));
    if (s == NULL) {
        return -1;
    }

    s->chairs = chairs;
    s->chairs_queue = calloc(chairs, sizeof(Job *));
    if (s->chairs_queue == NULL) {
        free(s);
        return -1;
    }

    pthread_mutex_init(&s->mutex, NULL);
    sem_init(&s->customers, 0, 0); 

    if (pthread_create(&s->barber, NULL, barber_thread, s) != 0) {
        sem_destroy(&s->customers);
        pthread_mutex_destroy(&s->mutex);
        free(s->chairs_queue);
        free(s);
        return -1;
    }

    *shop = s;
    return 0;
}

void barbershop_set_snapshots(Barbershop *shop, SnapshotBuffer *snapshots) {
    if (shop == NULL) {
        return;
    }

    pthread_mutex_lock(&shop->mutex);
    shop->snapshots = snapshots;
    pthread_mutex_unlock(&shop->mutex);
}

// Hilo manejador que recibió SUBMIT 
BarbershopResult barbershop_submit(Barbershop *shop, const JobParams *params,
    unsigned long *id) {

    Job *job;
    BarbershopResult result;

    if (shop == NULL || params == NULL || id == NULL) {
        return BARBERSHOP_FAILURE;
    }
    if (!valid_params(params)) {
        return BARBERSHOP_INVALID;
    }

    // Se prepara el trabajo antes de tomar el mutex 
    job = calloc(1, sizeof(Job));
    if (job == NULL) {
        return BARBERSHOP_FAILURE;
    }

    job->params = *params;
    job->state = JOB_QUEUED;

    pthread_mutex_lock(&shop->mutex); // Protege la revisión de las sillas

    if (shop->closing) {
        result = BARBERSHOP_CLOSED;
    } else if (shop->waiting == shop->chairs || shop->total_jobs == MAX_JOBS) {
        result = BARBERSHOP_BUSY; // No hay sillas y el cliente se va 
    } else {
        // Se sienta en una silla 
        shop->chairs_queue[(shop->head + shop->waiting) % shop->chairs] = job;
        shop->waiting++;
        shop->jobs[shop->total_jobs] = job;
        shop->total_jobs++;
        job->id = shop->total_jobs;
        *id = shop->total_jobs;
        result = BARBERSHOP_OK;
    }

    pthread_mutex_unlock(&shop->mutex);

    if (result == BARBERSHOP_OK) {
        sem_post(&shop->customers); // Avisa al barbero que hay un cliente
    } else {
        free(job);
    }

    return result;
}

int barbershop_query(Barbershop *shop, unsigned long id, JobInfo *info) {
    Job *job;
    Status *status;
    size_t i;

    if (shop == NULL || info == NULL) {
        return -1;
    }

    pthread_mutex_lock(&shop->mutex);

    if (id == 0 || id > shop->total_jobs) {
        pthread_mutex_unlock(&shop->mutex);
        return -1;
    }

    job = shop->jobs[id - 1];
    info->state = job->state;
    info->steps = job->params.steps;
    info->seconds = job->seconds;
    info->sum_abs_ez = job->sum_abs_ez;
    info->position = 0;
    info->step = 0;

    info->last_snapshot = 0;
    if(shop->snapshots != NULL && job->params.k > 0){
        info->last_snapshot = (job->params.steps / job->params.k) * job->params.k;

    }

    // Posición en la fila de espera
    for (i = 0; i < shop->waiting; i++) {
        if (shop->chairs_queue[(shop->head + i) % shop->chairs] == job) {
            info->position = i + 1;
        }
    }

    status = job->status;
    pthread_mutex_unlock(&shop->mutex);

    if (status != NULL) {
        status_get_step(status, &info->step);
    }

    return 0;
}

int barbershop_peek(Barbershop *shop, unsigned long id, size_t i, size_t j,
    size_t *step, double *ez) {

    Status *status;
    size_t nx;
    size_t ny;

    if (shop == NULL || step == NULL || ez == NULL) {
        return -1;
    }

    pthread_mutex_lock(&shop->mutex);
    if (id == 0 || id > shop->total_jobs) {
        pthread_mutex_unlock(&shop->mutex);
        return -1;
    }
    status = shop->jobs[id - 1]->status;
    nx = shop->jobs[id - 1]->params.nx;
    ny = shop->jobs[id - 1]->params.ny;
    pthread_mutex_unlock(&shop->mutex);

    if (i >= nx || j >= ny) {
        return -1;
    }

    // Todavía no empieza (o fue cancelado): aún no hay instantánea publicada.
    if (status == NULL) {
        *step = 0;
        *ez = 0.0;
        return 0;
    }

    return status_consult(status, step, i, j, ez);
}

void barbershop_shutdown(Barbershop *shop) {
    pthread_mutex_lock(&shop->mutex);

    if (shop->closing) { // Ya se cerró antes.
        pthread_mutex_unlock(&shop->mutex);
        return;
    }
    shop->closing = 1;

    // Los clientes que esperaban se van sin ser atendidos
    while (shop->waiting > 0) {
        shop->chairs_queue[shop->head]->state = JOB_ERROR;
        shop->head = (shop->head + 1) % shop->chairs;
        shop->waiting--;
    }

    pthread_mutex_unlock(&shop->mutex);

    // Despierta al barbero si estaba cortando, primero termina y lo espera
    sem_post(&shop->customers);
    pthread_join(shop->barber, NULL);
}

void barbershop_destroy(Barbershop *shop) {
    size_t i;

    if (shop == NULL) {
        return;
    }

    barbershop_shutdown(shop);

    for (i = 0; i < shop->total_jobs; i++) {
        status_destroy(shop->jobs[i]->status);
        free(shop->jobs[i]);
    }

    sem_destroy(&shop->customers);
    pthread_mutex_destroy(&shop->mutex);
    free(shop->chairs_queue);
    free(shop);
}