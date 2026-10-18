#define _POSIX_C_SOURCE 200809L

// Cola acotada de trabajos (problema de la barbería): llenar, vaciar, BUSY,
// orden FIFO, ausencia de pérdidas/duplicados y apagado.

#include "barbershop.h"
#include "test_util.h"

#define C 3

static JobParams params(size_t nx, size_t ny, size_t steps, size_t threads) {
    JobParams p;

    p.nx = nx;
    p.ny = ny;
    p.steps = steps;
    p.threads = threads;
    p.k = 0;
    p.sx = nx / 2;
    p.sy = ny / 2;
    return p;
}

// Trabajo "largo": mantiene ocupado al barbero mientras se llenan las sillas.
static JobParams long_job(void) {
    return params(400, 400, 1000, 2);
}

static JobParams short_job(void) {
    return params(24, 24, 20, 2);
}

static int wait_state(Barbershop *shop, unsigned long id, JobState state, double timeout) {
    double t0 = now_s();
    JobInfo info;

    while (now_s() - t0 < timeout) {
        if (barbershop_query(shop, id, &info) == 0 && info.state == state) {
            return 1;
        }
        sleep_ms(2);
    }
    return 0;
}

static int is_finished(JobState s) {
    return s == JOB_DONE || s == JOB_ERROR;
}

// Espera a que terminen los trabajos 1..n verificando FIFO en cada muestra:
// si un trabajo j ya arrancó, todos los anteriores deben haber terminado.
// Se muestrea de mayor a menor id porque los estados solo avanzan.
static int wait_all_fifo(Barbershop *shop, unsigned long n, double timeout) {
    double t0 = now_s();
    int fifo_ok = 1;

    while (now_s() - t0 < timeout) {
        int started_seen = 0;
        int all_done = 1;
        unsigned long id;

        for (id = n; id >= 1; id--) {
            JobInfo info;

            barbershop_query(shop, id, &info);
            if (started_seen && !is_finished(info.state)) {
                fifo_ok = 0;
            }
            if (info.state == JOB_RUNNING || is_finished(info.state)) {
                started_seen = 1;
            }
            if (!is_finished(info.state)) {
                all_done = 0;
            }
        }
        if (all_done) {
            return fifo_ok ? 1 : -1;
        }
        sleep_ms(2);
    }
    return 0; // Timeout
}

int main(void) {
    Barbershop *shop = NULL;
    JobParams lp = long_job();
    JobParams sp = short_job();
    JobParams bad;
    JobInfo info;
    unsigned long id = 0;
    unsigned long ids[8];
    double t0;
    int r;

    TEST_START("barbershop (cola acotada)");

    CHECK(barbershop_init(&shop, 0) == -1);
    CHECK(barbershop_init(NULL, 3) == -1);
    CHECK(barbershop_init(&shop, C) == 0);

    // Parámetros inválidos: se rechazan sin ocupar silla ni consumir id
    bad = sp; bad.threads = 0;       CHECK(barbershop_submit(shop, &bad, &id) == BARBERSHOP_INVALID);
    bad = sp; bad.threads = 23;      CHECK(barbershop_submit(shop, &bad, &id) == BARBERSHOP_INVALID); // > nx-2
    bad = sp; bad.nx = 2;            CHECK(barbershop_submit(shop, &bad, &id) == BARBERSHOP_INVALID);
    bad = sp; bad.steps = 0;         CHECK(barbershop_submit(shop, &bad, &id) == BARBERSHOP_INVALID);
    bad = sp; bad.sx = 0;            CHECK(barbershop_submit(shop, &bad, &id) == BARBERSHOP_INVALID);
    bad = sp; bad.sy = sp.ny - 1;    CHECK(barbershop_submit(shop, &bad, &id) == BARBERSHOP_INVALID);
    bad = sp; bad.k = sp.steps + 1;  CHECK(barbershop_submit(shop, &bad, &id) == BARBERSHOP_INVALID);
    CHECK(barbershop_submit(NULL, &sp, &id) == BARBERSHOP_FAILURE);
    CHECK(barbershop_query(shop, 1, &info) == -1); // Aún no hay trabajos

    // El barbero toma el trabajo largo: queda dormido -> ocupado
    CHECK(barbershop_submit(shop, &lp, &ids[0]) == BARBERSHOP_OK);
    CHECK(ids[0] == 1);
    CHECK(wait_state(shop, ids[0], JOB_RUNNING, 10.0));

    // Llenar las C sillas
    for (r = 1; r <= C; r++) {
        CHECK(barbershop_submit(shop, &sp, &ids[r]) == BARBERSHOP_OK);
        CHECK(ids[r] == (unsigned long)r + 1); // ids consecutivos, sin saltos
    }
    for (r = 1; r <= C; r++) {
        CHECK(barbershop_query(shop, ids[r], &info) == 0);
        CHECK(info.state == JOB_QUEUED);
        CHECK(info.position == (size_t)r); // Orden de la fila
    }

    // Sin sillas: BUSY inmediato, sin bloquear y sin consumir id
    t0 = now_s();
    CHECK(barbershop_submit(shop, &sp, &id) == BARBERSHOP_BUSY);
    CHECK(barbershop_submit(shop, &sp, &id) == BARBERSHOP_BUSY);
    CHECK(now_s() - t0 < 0.5);
    CHECK(barbershop_query(shop, 5, &info) == -1);

    // El progreso del trabajo largo es coherente mientras corre
    barbershop_query(shop, ids[0], &info);
    CHECK(info.state == JOB_RUNNING || info.state == JOB_DONE);
    CHECK(info.step <= info.steps);

    // Vaciar la fila respetando FIFO
    CHECK(wait_all_fifo(shop, 4, 60.0) == 1);
    for (id = 1; id <= 4; id++) {
        CHECK(barbershop_query(shop, id, &info) == 0);
        CHECK(info.state == JOB_DONE);
        CHECK(info.seconds > 0.0);
        CHECK(info.sum_abs_ez > 0.0);
        CHECK(info.step == info.steps); // Progreso final publicado
    }

    // No se perdió ni se duplicó nada: exactamente 4 ids válidos
    CHECK(barbershop_query(shop, 0, &info) == -1);
    CHECK(barbershop_query(shop, 5, &info) == -1);

    // Con la fila vacía se vuelve a aceptar trabajo
    CHECK(barbershop_submit(shop, &sp, &id) == BARBERSHOP_OK);
    CHECK(id == 5);
    CHECK(wait_state(shop, 5, JOB_DONE, 10.0));

    // PEEK sobre un trabajo terminado: última instantánea (paso final)
    {
        size_t step = 0;
        double ez = 1.0;

        CHECK(barbershop_peek(shop, 5, sp.sx, sp.sy, &step, &ez) == 0);
        CHECK(step == sp.steps);
        CHECK(barbershop_peek(shop, 5, sp.nx, 0, &step, &ez) == -1);
        CHECK(barbershop_peek(shop, 5, 0, sp.ny, &step, &ez) == -1);
        CHECK(barbershop_peek(shop, 99, 0, 0, &step, &ez) == -1);
    }
    barbershop_destroy(shop);

    // ---- Apagado con trabajo en ejecución y trabajos en cola
    CHECK(barbershop_init(&shop, C) == 0);
    CHECK(barbershop_submit(shop, &lp, &ids[0]) == BARBERSHOP_OK);
    CHECK(wait_state(shop, ids[0], JOB_RUNNING, 10.0));
    CHECK(barbershop_submit(shop, &sp, &ids[1]) == BARBERSHOP_OK);
    CHECK(barbershop_submit(shop, &sp, &ids[2]) == BARBERSHOP_OK);

    barbershop_shutdown(shop); // Espera a que termine el trabajo actual
    barbershop_query(shop, ids[0], &info);
    CHECK(info.state == JOB_DONE);          // El actual se completó
    barbershop_query(shop, ids[1], &info);
    CHECK(info.state == JOB_ERROR);         // Los que esperaban se cancelan
    barbershop_query(shop, ids[2], &info);
    CHECK(info.state == JOB_ERROR);
    CHECK(barbershop_submit(shop, &sp, &id) == BARBERSHOP_CLOSED);
    barbershop_shutdown(shop);              // Idempotente
    barbershop_destroy(shop);

    TEST_END();
}