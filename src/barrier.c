#include "barrier.h"

int barrier_init(barrier_t *b, int num_hilos) {

    if (b == NULL || num_hilos <= 0) {
        return -1; //Por ahora si hay algún error en la inicialización retornamos -1
        //pero podriamos usar una biblioteca que retorne códigos de error especificos o dejarlo así
    }

    b->num_hilos = num_hilos;
    b->contador = 0;
    b->fase = 0;

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
    b->contador++;

    //Si es el último hilo en llegar a la barrera, reinicia el contador y alterna la fase.
    if (b->contador == b->num_hilos) {
        b->contador = 0;
        b->fase = !(b->fase); 
        pthread_cond_broadcast(&b->cond); // Despierta a todos los hilos que están esperando en la barrera
        pthread_mutex_unlock(&b->mutex); 
        return 0;
    } else {
        int fase_actual = b->fase;
        while (b->fase == fase_actual) {
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