#define _POSIX_C_SOURCE 200809L

// Barrera reutilizable: N hilos x M rondas; ningún hilo pasa la ronda r
// antes de que los N hayan llegado a ella.

#include "barrier.h"
#include "test_util.h"

#include <pthread.h>

#define N 6
#define M 400

static barrier_t bar;
static pthread_mutex_t mtx = PTHREAD_MUTEX_INITIALIZER;
static int arrived[M];

typedef struct {
    int id;
    int bad;
} Worker;

static void *worker(void *arg) {
    Worker *w = arg;
    int r;

    for (r = 0; r < M; r++) {
        // Ritmos distintos: algunos hilos se atrasan, otros corren.
        if ((r + w->id) % 11 == 0) {
            sleep_us(200);
        }
        if (w->id == 0 && r % 50 == 0) {
            sleep_ms(2);
        }

        pthread_mutex_lock(&mtx);
        arrived[r]++;
        pthread_mutex_unlock(&mtx);

        barrier_wait(&bar);

        // Al salir de la barrera de la ronda r, los N ya deben haber llegado.
        pthread_mutex_lock(&mtx);
        if (arrived[r] != N) {
            w->bad++;
        }
        pthread_mutex_unlock(&mtx);
    }
    return NULL;
}

static barrier_t b3;
static double waited[3];

static void *blocker(void *arg) {
    long id = (long)arg;
    double t0;

    if (id == 2) {
        sleep_ms(200); // El último en llegar
    }
    t0 = now_s();
    barrier_wait(&b3);
    waited[id] = now_s() - t0;
    return NULL;
}

int main(void) {
    pthread_t th[N];
    Worker w[N];
    barrier_t one;
    pthread_t t3[3];
    int i;

    TEST_START("barrier");

    // Argumentos inválidos
    CHECK(barrier_init(NULL, 3) == -1);
    CHECK(barrier_init(&one, 0) == -1);
    CHECK(barrier_init(&one, -2) == -1);
    CHECK(barrier_wait(NULL) == -1);

    // N hilos x M rondas con la MISMA barrera (reutilizable)
    CHECK(barrier_init(&bar, N) == 0);
    for (i = 0; i < N; i++) {
        w[i].id = i;
        w[i].bad = 0;
        CHECK(pthread_create(&th[i], NULL, worker, &w[i]) == 0);
    }
    for (i = 0; i < N; i++) {
        pthread_join(th[i], NULL);
        CHECK(w[i].bad == 0);
    }
    for (i = 0; i < M; i++) {
        CHECK(arrived[i] == N);
    }
    barrier_destroy(&bar);

    // La barrera realmente bloquea: los dos primeros esperan al tercero
    CHECK(barrier_init(&b3, 3) == 0);
    for (i = 0; i < 3; i++) {
        pthread_create(&t3[i], NULL, blocker, (void *)(long)i);
    }
    for (i = 0; i < 3; i++) {
        pthread_join(t3[i], NULL);
    }
    CHECK(waited[0] >= 0.15);
    CHECK(waited[1] >= 0.15);
    CHECK(waited[2] < 0.15);
    barrier_destroy(&b3);

    // Con un solo hilo nunca bloquea
    CHECK(barrier_init(&one, 1) == 0);
    for (i = 0; i < 1000; i++) {
        CHECK(barrier_wait(&one) == 0);
    }
    barrier_destroy(&one);

    TEST_END();
}