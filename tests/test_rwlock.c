#define _POSIX_C_SOURCE 200809L

// Candado lectores-escritor de status.c.
// Se incluye status.c directamente para poder probar reader_entry/exit y
// writer_entry/exit (son static). Por eso este test no se enlaza con status.o.

#include "../src/status.c"
#include "test_util.h"

static Status *st;
static pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;

// ---------------------------------------------------------------- lectores
static int inside;
static int max_inside;

static void *reader_together(void *arg) {
    double t0 = now_s();
    int reached = 0;

    (void)arg;
    reader_entry(st);
    pthread_mutex_lock(&m);
    inside++;
    if (inside > max_inside) {
        max_inside = inside;
    }
    pthread_mutex_unlock(&m);

    // Espera a que todos los lectores estén dentro al mismo tiempo
    while (now_s() - t0 < 3.0) {
        pthread_mutex_lock(&m);
        reached = (inside == 5);
        pthread_mutex_unlock(&m);
        if (reached) {
            break;
        }
        sleep_us(500);
    }
    reader_exit(st);
    return (void *)(long)reached;
}

// --------------------------------------------------------------- exclusión
static _Atomic int flag_a;

static void *writer_once(void *arg) {
    writer_entry(st);
    flag_a = 1;
    writer_exit(st);
    return arg;
}

static void *reader_once(void *arg) {
    reader_entry(st);
    flag_a = 1;
    reader_exit(st);
    return arg;
}

// ---------------------------------------------------------------- inanición
static _Atomic int stop_readers;
static long reads_done;

static void *busy_reader(void *arg) {
    long id = (long)arg;

    sleep_ms(id); // Escalonados para que siempre haya alguno dentro
    while (!stop_readers) {
        reader_entry(st);
        sleep_ms(5);
        reader_exit(st);
        pthread_mutex_lock(&m);
        reads_done++;
        pthread_mutex_unlock(&m);
    }
    return NULL;
}

// Operaciones públicas de Status ejecutadas desde otro hilo
static double small_grid[16];

static void *do_publish(void *arg) {
    (void)arg;
    status_publish(st, 7, small_grid);
    flag_a = 1;
    return NULL;
}

static void *do_set_step(void *arg) {
    (void)arg;
    status_set_step(st, 7);
    flag_a = 1;
    return NULL;
}

static void *do_consult(void *arg) {
    size_t step;
    double ez;

    (void)arg;
    status_consult(st, &step, 0, 0, &ez);
    flag_a = 1;
    return NULL;
}

static void *do_get_step(void *arg) {
    size_t step;

    (void)arg;
    status_get_step(st, &step);
    flag_a = 1;
    return NULL;
}

static _Atomic int writes_done;

static void *writer_many(void *arg) {
    int w;

    (void)arg;
    for (w = 0; w < 20; w++) {
        writer_entry(st);
        writer_exit(st);
    }
    writes_done = 1;
    return NULL;
}

// ---------------------------------------------------------------- integridad
#define GN 1000
static _Atomic int stop_integrity;
static int inconsistent;

static void *publisher(void *arg) {
    double *grid = malloc(GN * GN * sizeof(double));
    size_t v, i;

    (void)arg;
    for (v = 1; v <= 400; v++) {
        for (i = 0; i < GN * GN; i++) {
            grid[i] = (double)v;
        }
        status_publish(st, v, grid);
    }
    free(grid);
    stop_integrity = 1;
    return NULL;
}

static void *consulter(void *arg) {
    (void)arg;
    while (!stop_integrity) {
        size_t step = 0;
        double ez = 0.0;

        // memcpy escribe primero la celda (0,0) y el paso se actualiza al
        // final: sin exclusión mutua se vería ez != step durante todo el memcpy.
        if (status_consult(st, &step, 0, 0, &ez) == 0) {
            if (ez != (double)step) {
                pthread_mutex_lock(&m);
                inconsistent++;
                pthread_mutex_unlock(&m);
            }
        }
    }
    return NULL;
}

int main(void) {
    pthread_t th[8];
    void *ret;
    int i;
    int all_together = 1;

    TEST_START("rwlock (status)");

    CHECK(status_init(&st, 4, 4) == 0);

    // 1) Varios lectores simultáneos
    for (i = 0; i < 5; i++) {
        pthread_create(&th[i], NULL, reader_together, NULL);
    }
    for (i = 0; i < 5; i++) {
        pthread_join(th[i], &ret);
        if (!ret) {
            all_together = 0;
        }
    }
    CHECK(all_together);
    CHECK(max_inside == 5);

    // 2a) Un escritor espera a que salga el lector
    reader_entry(st);
    flag_a = 0;
    pthread_create(&th[0], NULL, writer_once, NULL);
    sleep_ms(150);
    CHECK(flag_a == 0);
    reader_exit(st);
    pthread_join(th[0], NULL);
    CHECK(flag_a == 1);

    // 2b) Mientras el escritor está dentro, ni lectores ni otro escritor entran
    writer_entry(st);
    flag_a = 0;
    pthread_create(&th[0], NULL, reader_once, NULL);
    pthread_create(&th[1], NULL, writer_once, NULL);
    sleep_ms(150);
    CHECK(flag_a == 0);
    writer_exit(st);
    pthread_join(th[0], NULL);
    pthread_join(th[1], NULL);
    CHECK(flag_a == 1);

    // 2c) Las operaciones públicas respetan el candado: publicar / avanzar el
    //     paso esperan a los lectores; consultar espera al escritor.
    {
        void *(*writers[2])(void *) = {do_publish, do_set_step};
        void *(*readers[2])(void *) = {do_consult, do_get_step};

        for (i = 0; i < 2; i++) {
            reader_entry(st);
            flag_a = 0;
            pthread_create(&th[0], NULL, writers[i], NULL);
            sleep_ms(150);
            CHECK(flag_a == 0);      // Bloqueado mientras hay un lector dentro
            reader_exit(st);
            pthread_join(th[0], NULL);
            CHECK(flag_a == 1);

            writer_entry(st);
            flag_a = 0;
            pthread_create(&th[0], NULL, readers[i], NULL);
            sleep_ms(150);
            CHECK(flag_a == 0);      // Bloqueado mientras el escritor está dentro
            writer_exit(st);
            pthread_join(th[0], NULL);
            CHECK(flag_a == 1);
        }
    }

    // 3) El escritor no sufre inanición con lectores que se solapan siempre
    stop_readers = 0;
    for (i = 0; i < 4; i++) {
        pthread_create(&th[i], NULL, busy_reader, (void *)(long)i);
    }
    sleep_ms(50);
    {
        double t0 = now_s();

        writes_done = 0;
        pthread_create(&th[4], NULL, writer_many, NULL);
        while (!writes_done && now_s() - t0 < 5.0) {
            sleep_ms(5);
        }
        CHECK(writes_done); // Con inanición del escritor esto nunca termina
        if (!writes_done) {
            stop_readers = 1; // Con inanición, esto libera al escritor para poder terminar
        }
        pthread_join(th[4], NULL);
    }
    // Los lectores también siguen avanzando (no se bloquean para siempre)
    {
        long before;

        pthread_mutex_lock(&m);
        before = reads_done;
        pthread_mutex_unlock(&m);
        sleep_ms(100);
        pthread_mutex_lock(&m);
        CHECK(reads_done > before);
        pthread_mutex_unlock(&m);
    }
    stop_readers = 1;
    for (i = 0; i < 4; i++) {
        pthread_join(th[i], NULL);
    }

    status_destroy(st);

    // 4) Integridad: nadie lee una publicación a medias
    CHECK(status_init(&st, GN, GN) == 0);
    pthread_create(&th[0], NULL, publisher, NULL);
    for (i = 1; i <= 4; i++) {
        pthread_create(&th[i], NULL, consulter, NULL);
    }
    for (i = 0; i <= 4; i++) {
        pthread_join(th[i], NULL);
    }
    CHECK(inconsistent == 0);
    status_destroy(st);

    // 5) Progreso barato y consultas fuera de rango
    CHECK(status_init(&st, 4, 4) == 0);
    {
        size_t step = 99;
        double ez = 0.0;

        CHECK(status_get_step(st, &step) == 0 && step == 0);
        CHECK(status_set_step(st, 42) == 0);
        CHECK(status_get_step(st, &step) == 0 && step == 42);
        CHECK(status_consult(st, &step, 4, 0, &ez) == -1);
        CHECK(status_consult(st, &step, 0, 4, &ez) == -1);
        CHECK(status_consult(st, &step, 3, 3, &ez) == 0);
    }
    status_destroy(st);

    TEST_END();
}