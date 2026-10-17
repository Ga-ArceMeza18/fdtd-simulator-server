
#include "status.h"

#include <errno.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

struct Status {
    size_t nx;
    size_t ny;
    size_t step_published;
    double *ez_published;

    // Candado lectores-escritor con prioridad para escritores.
    // Siguiendo la solución de "Readers and Writers" del libro "Little Book of Semaphores" de Allen B. Downey.
    sem_t resource; // semáforo para acceder al recurso compartido
    sem_t readTry; // semáforo para permitir que los lectores entren solo si no hay escritores esperando
    pthread_mutex_t rmutex; // read mutex
    pthread_mutex_t wmutex; // write mutex

    size_t read_count;
    size_t write_count;
};

// Reintenta sem_wait si una señal interrumpe la llamada.
static int retry_sem_wait(sem_t *sem) {
    int result;

    do {
        result = sem_wait(sem);
    } while (result == -1 && errno == EINTR); // Reintenta si la llamada fue interrumpida por una señal.
    
    return result;
}

// Entra como lector. Varios lectores pueden consultar simultáneamente.
static int reader_entry(Status *status) {

    // Intenta adquirir el semáforo readTry para asegurarse de que no hay escritores esperando.
    // Revisa si el semáforo se adquiere correctamente. Si no, retorna un error.
    if (retry_sem_wait(&status->readTry) == -1) {
        return -1;
    }

    // Bloquea el rmutex para proteger el acceso a read_count.
    // Revisa si el mutex se bloquea correctamente. Si no, libera el semáforo readTry y retorna un error.
    if (pthread_mutex_lock(&status->rmutex) != 0) {
        sem_post(&status->readTry);
        return -1;
    }

    // Si no hay lectores activos, intenta adquirir el semáforo resource para acceder al recurso compartido.
    if (status->read_count == 0) {
        // Revisa si el semáforo resource se adquiere correctamente. Si no, desbloquea el mutex rmutex, libera el semáforo readTry y retorna un error.
        if (retry_sem_wait(&status->resource) == -1) {
            pthread_mutex_unlock(&status->rmutex);
            sem_post(&status->readTry);
            return -1;
        }
    }

    status->read_count++;

    pthread_mutex_unlock(&status->rmutex);
    sem_post(&status->readTry);

    return 0;
}

// Sale como lector. El último lector libera el recurso compartido.
static int reader_exit(Status *status) {

    // Bloquea el rmutex para proteger el acceso a read_count.
    // Revisa si el mutex se bloquea correctamente. Si no, retorna un error.
    if (pthread_mutex_lock(&status->rmutex) != 0) {
        return -1;
    }

    status->read_count--;

    int resultado = 0;

    if (status->read_count == 0) {
        // Revisa si el semáforo resource se libera correctamente. Si no, establece resultado en -1.
        if (sem_post(&status->resource) == -1) {
            resultado = -1;
        }
    }

    // Desbloquea el rmutex y revisa si se desbloquea correctamente. Si no, establece resultado en -1.
    if (pthread_mutex_unlock(&status->rmutex) != 0) {
        resultado = -1;
    }

    return resultado;
}

// Entra como escritor. Los nuevos lectores dejan de entrar cuando el primer escritor está esperando.
static int writer_entry(Status *status) {
    // Bloquea el wmutex para proteger el acceso a write_count.
    // Revisa si el mutex se bloquea correctamente. Si no, retorna un error.
    if (pthread_mutex_lock(&status->wmutex) != 0) {
        return -1;
    }

    if (status->write_count == 0) {
        // Intenta adquirir el semáforo readTry para evitar que nuevos lectores entren mientras hay escritores esperando.
        // Revisa si el semáforo readTry se adquiere correctamente. Si no, desbloquea el wmutex y retorna un error.
        if (retry_sem_wait(&status->readTry) == -1) {
            pthread_mutex_unlock(&status->wmutex);
            return -1;
        }
    }

    status->write_count++;

    // Desbloquea el wmutex y revisa si se desbloquea correctamente. Si no, retorna un error.
    if (pthread_mutex_unlock(&status->wmutex) != 0) {
        return -1;
    }

    if (retry_sem_wait(&status->resource) == -1) {
        // Recupera el estado si no se pudo adquirir el recurso.
        pthread_mutex_lock(&status->wmutex);
        status->write_count--;

        // Si no hay escritores esperando, libera el semáforo readTry para permitir que los lectores entren.
        if (status->write_count == 0) {
            sem_post(&status->readTry);
        }

        pthread_mutex_unlock(&status->wmutex);
        return -1;
    }

    return 0;
}

// Sale como escritor y permite entrar a nuevos lectores cuando ya no quedan escritores esperando.
static int writer_exit(Status *status) {
    int resultado = 0;

    // Libera el semáforo resource para permitir que otros lectores o escritores accedan al recurso compartido.
    if (sem_post(&status->resource) == -1) {
        resultado = -1;
    }

    // Bloquea el wmutex para proteger el acceso a write_count.
    // Revisa si el mutex se bloquea correctamente. Si no, retorna un error.
    if (pthread_mutex_lock(&status->wmutex) != 0) {
        return -1;
    }

    status->write_count--;

    // Si no hay escritores esperando, libera el semáforo readTry para permitir que los lectores entren.
    if (status->write_count == 0) {
        // Revisa si el semáforo readTry se libera correctamente. Si no, establece resultado en -1.
        if (sem_post(&status->readTry) == -1) {
            resultado = -1;
        }
    }

    if (pthread_mutex_unlock(&status->wmutex) != 0) {
        resultado = -1;
    }

    return resultado;
}

int status_init(Status **status, size_t nx, size_t ny) {
    if (status == NULL || nx == 0 || ny == 0) {
        return -1;
    }

    *status = NULL;

    // Revisa si la multiplicación de nx y ny causará un desbordamiento de tamaño. Si es así, retorna un error.
    if (nx > SIZE_MAX / ny) {
        return -1;
    }

    size_t cantidad = nx * ny;

    // Revisa si la multiplicación de cantidad y sizeof(double) causará un desbordamiento de tamaño. Si es así, retorna un error.
    if (cantidad > SIZE_MAX / sizeof(double)) {
        return -1;
    }

    Status *new_status = calloc(1, sizeof(Status));

    // Revisa si la asignación de memoria para la estructura Status fue exitosa. Si no, retorna un error.
    if (new_status == NULL) {
        return -1;
    }

    new_status->nx = nx;
    new_status->ny = ny;
    new_status->step_published = 0;

    new_status->ez_published = calloc(cantidad, sizeof(double));

    // Revisa si la asignación de memoria para ez_published fue exitosa. Si no, libera la memoria de la estructura Status y retorna un error.
    if (new_status->ez_published == NULL) {
        free(new_status);
        return -1;
    }

    // Inicializa los semáforos y mutexes. Si falla alguno, libera la memoria asignada y retorna un error.
    // Revisa si la inicialización del semáforo resource fue exitosa. Si no, libera la memoria de ez_published y la estructura Status, y retorna un error.
    if (sem_init(&new_status->resource, 0, 1) == -1) {
        free(new_status->ez_published);
        free(new_status);
        return -1;
    }

    // Inicializa el semáforo readTry con un valor inicial de 1, lo que permite que los lectores entren si no hay escritores esperando.
    // Revisa si la inicialización del semáforo readTry fue exitosa. Si no, destruye el semáforo resource, libera la memoria de ez_published y la estructura Status, y retorna un error.
    if (sem_init(&new_status->readTry, 0, 1) == -1) {
        sem_destroy(&new_status->resource);
        free(new_status->ez_published);
        free(new_status);
        return -1;
    }

    // Inicializa los mutexes para proteger el acceso a read_count y write_count.
    // Revisa si la inicialización del mutex rmutex fue exitosa. Si no, destruye los semáforos readTry y resource, libera la memoria de ez_published y la estructura Status, y retorna un error.
    if (pthread_mutex_init(&new_status->rmutex, NULL) != 0) {
        sem_destroy(&new_status->readTry);
        sem_destroy(&new_status->resource);
        free(new_status->ez_published);
        free(new_status);
        return -1;
    }

    // Inicializa el mutex wmutex para proteger el acceso a write_count y garantizar la exclusión mutua entre escritores.
    // Revisa si la inicialización del mutex wmutex fue exitosa. Si no, destruye el mutex rmutex, los semáforos readTry y resource, libera la memoria de ez_published y la estructura Status, y retorna un error.
    if (pthread_mutex_init(&new_status->wmutex, NULL) != 0) {
        pthread_mutex_destroy(&new_status->rmutex);
        sem_destroy(&new_status->readTry);
        sem_destroy(&new_status->resource);
        free(new_status->ez_published);
        free(new_status);
        return -1;
    }

    *status = new_status;
    return 0;
}

int status_publish(Status *status, size_t step, const double *ez) {
    size_t size;

    // Revisa si los punteros son nulos
    if (status == NULL || ez == NULL) {
        return -1;
    }
    
    size = status->nx * status->ny; // Calcula la cantidad de elementos en el arreglo Ez

    // Revisa si la cantidad es cero o mayor que el tamaño máximo permitido, lo que indicaría un error en las dimensiones de la simulación
    if (size == 0 || size > SIZE_MAX / sizeof(double)) {
        return -1;
    }

    // Revisa si el paso es menor que el paso publicado actual
    if (writer_entry(status) != 0) {
        return -1;
    }

    memcpy(status->ez_published, ez, size * sizeof(double)); // Copia los valores de Ez al arreglo publicado en el estado
    status->step_published = step; // Actualiza el paso publicado en el estado

    return writer_exit(status);
}

int status_consult(Status *status, size_t *step,
            size_t i, size_t j, double *ez_value) {

    // Revisa si los punteros son nulos
    if (status == NULL || step == NULL) {
        return -1;
    }

    // Revisa si los índices i y j están fuera de los límites del arreglo Ez publicado
    if (ez_value != NULL && (i >= status->nx || j >= status->ny)) {
        return -1;
    }

    // Revisa si se puede entrar como lector. Si no, retorna un error.
    if (reader_entry(status) != 0) {
        return -1;
    }

    *step = status->step_published;

    if (ez_value != NULL) {
        *ez_value = status->ez_published[i * status->ny + j];
    }

    return reader_exit(status);
}

void status_destroy(Status *status) {
    if (status == NULL) {
        return;
    }

    // El servidor al apagarse debe garantizar previamente que ningún hilo siga usando este estado.
    // Liberar todos lo que fue creado en status_init
    pthread_mutex_destroy(&status->wmutex);
    pthread_mutex_destroy(&status->rmutex);
    sem_destroy(&status->readTry);
    sem_destroy(&status->resource);

    free(status->ez_published);
    free(status);
}
