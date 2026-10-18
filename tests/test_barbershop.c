// Prueba de la cola acotada de trabajos (barbería): llenar, BUSY, FIFO, vaciar y apagado.

#define _POSIX_C_SOURCE 200809L

#include "barbershop.h"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static int failures = 0;

#define CHECK(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "FALLO linea %d: %s\n", __LINE__, #cond); \
        failures++; \
    } \
} while (0)

static JobParams job(size_t n, size_t steps) {
    JobParams p = { n, n, steps, 2, 0, n / 2, n / 2 };
    return p;
}

// Espera (máximo ~60 s) a que el trabajo llegue al estado indicado.
static int wait_state(Barbershop *shop, unsigned long id, JobState state) {
    struct timespec ten_ms = { 0, 10000000L };
    JobInfo info;
    int i;

    for (i = 0; i < 6000; i++) {
        if (barbershop_query(shop, id, &info) == 0 && info.state == state) {
            return 1;
        }
        nanosleep(&ten_ms, NULL);
    }
    return 0;
}

int main(void) {
    enum { C = 3 };
    Barbershop *shop;
    JobParams large = job(300, 600); // tarda lo suficiente para verlo en RUNNING
    JobParams small = job(16, 10);
    JobParams bad = job(10, 10);
    unsigned long first;
    unsigned long ids[C];
    unsigned long id;
    JobInfo info;
    size_t i;

    // Llenar, BUSY al exceder C, orden FIFO y vaciar.
    CHECK(barbershop_init(&shop, C) == 0);

    bad.threads = 9; // hilos > nx - 2
    CHECK(barbershop_submit(shop, &bad, &id) == BARBERSHOP_INVALID);

    CHECK(barbershop_submit(shop, &large, &first) == BARBERSHOP_OK);
    CHECK(wait_state(shop, first, JOB_RUNNING)); // el barbero lo tomó: la cola está vacía

    for (i = 0; i < C; i++) {
        CHECK(barbershop_submit(shop, &small, &ids[i]) == BARBERSHOP_OK);
        CHECK(ids[i] == first + i + 1);
    }
    CHECK(barbershop_submit(shop, &small, &id) == BARBERSHOP_BUSY); // cola llena

    for (i = 0; i < C; i++) {
        CHECK(barbershop_query(shop, ids[i], &info) == 0);
        CHECK(info.state == JOB_QUEUED);
        CHECK(info.position == i + 1); // FIFO
    }

    for (i = 0; i < C; i++) {
        CHECK(wait_state(shop, ids[i], JOB_DONE));
        CHECK(barbershop_query(shop, ids[i], &info) == 0);
        CHECK(info.step == small.steps);
    }
    CHECK(barbershop_query(shop, 999, &info) == -1);

    barbershop_destroy(shop);

    // Apagado con trabajos en cola: el actual termina, los demás se cancelan.
    CHECK(barbershop_init(&shop, C) == 0);
    CHECK(barbershop_submit(shop, &large, &first) == BARBERSHOP_OK);
    CHECK(wait_state(shop, first, JOB_RUNNING));
    for (i = 0; i < C; i++) {
        CHECK(barbershop_submit(shop, &small, &ids[i]) == BARBERSHOP_OK);
    }

    barbershop_shutdown(shop);

    CHECK(barbershop_query(shop, first, &info) == 0);
    CHECK(info.state == JOB_DONE);
    for (i = 0; i < C; i++) {
        CHECK(barbershop_query(shop, ids[i], &info) == 0);
        CHECK(info.state == JOB_ERROR);
    }
    CHECK(barbershop_submit(shop, &small, &id) == BARBERSHOP_CLOSED);

    barbershop_destroy(shop);

    printf("test_barbershop: %s\n", failures == 0 ? "OK" : "FALLO");
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
