#ifndef BARRIER_H
#define BARRIER_H

#include <pthread.h>

typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    int contador; //cuenta cuantos hilos llegaron a la barrera
    int fase; //alterna entre 0 y 1 para saber cuando todos los hilos ya llegaron a la barrera
    int num_hilos; //cuantos hilos deben llegar a la barrera para que se liberen todos
} barrier_t;

int barrier_init(barrier_t *b, int num_hilos);
int barrier_wait(barrier_t *b);
void barrier_destroy(barrier_t *b);

#endif