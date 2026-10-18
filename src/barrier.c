#include "barrier.h"

int barrier_init(barrier_t *b, int num_threads) {

    if (b == NULL || num_threads <= 0) {
        return -1; //Por ahora si hay algún error en la inicialización retornamos -1
        //pero podriamos usar una biblioteca que retorne códigos de error especificos o dejarlo así
    }

    b->num_threads = num_threads;
    b->count = 0;
    b->phase = 0;

    if (pthread_mutex_init(&b->mutex, NULL) != 0) {
        return -1;
    }

    if (pthread_cond_init(&b->cond, NULL) != 0) {
        pthread_mutex_destroy(&b->mutex);
        return -1;
    }

    return 0;
}

int barrier_wait(barrier_t *b) {

    if (b == NULL) {
        return -1;
    }

    pthread_mutex_lock(&b->mutex);
    b->count++;

    //Si es el último hilo en llegar a la barrera, reinicia el contador y alterna la fase.
    if (b->count == b->num_threads) {
        b->count = 0;
        b->phase = !(b->phase); 
        pthread_cond_broadcast(&b->cond); // Despierta a todos los hilos que están esperando en la barrera
        pthread_mutex_unlock(&b->mutex); 
        return 0;
    } else {
        int phase_actual = b->phase;
        while (b->phase == phase_actual) {
            pthread_cond_wait(&b->cond, &b->mutex);
        }
        pthread_mutex_unlock(&b->mutex);
        return 0;
    }
}

void barrier_destroy(barrier_t *b) {
    if (b != NULL) {
        pthread_cond_destroy(&b->cond);
        pthread_mutex_destroy(&b->mutex);
    }
}