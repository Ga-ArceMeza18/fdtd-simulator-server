#define _POSIX_C_SOURCE 200809L

// Determinismo: la malla final con T = 1, 2, 3, 4, 5, 7, 8 hilos debe ser
// idéntica bit a bit (memcmp) a la de 1 hilo. Las filas interiores (255) no son
// divisibles entre la mayoría de esos T.
//
// Compilado con -DSIN_BARRERA este test DEBE fallar (ver "make test"): así se
// demuestra que la prueba detecta la condición de carrera.

#include "fdtd_solver.h"
#include "test_util.h"

#define NX 257
#define NY 301
#define STEPS 400
#define REPS 3

typedef struct {
    double *ez, *hx, *hy;
} Result;

static int run_sim(size_t threads, Result *out) {
    struct FDTD f;
    size_t nez = NX * NY, nhx = NX * (NY - 1), nhy = (NX - 1) * NY;

    if (fdtd_init(&f, NX, NY, STEPS, threads, NX / 2, NY / 2) != 0) {
        return -1;
    }
    if (fdtd_run(&f) != 0) {
        fdtd_destroy(&f);
        return -1;
    }
    out->ez = malloc(nez * sizeof(double));
    out->hx = malloc(nhx * sizeof(double));
    out->hy = malloc(nhy * sizeof(double));
    memcpy(out->ez, f.ez, nez * sizeof(double));
    memcpy(out->hx, f.hx, nhx * sizeof(double));
    memcpy(out->hy, f.hy, nhy * sizeof(double));
    fdtd_destroy(&f);
    return 0;
}

static void free_result(Result *r) {
    free(r->ez);
    free(r->hx);
    free(r->hy);
}

static int identical(const Result *a, const Result *b) {
    return memcmp(a->ez, b->ez, NX * NY * sizeof(double)) == 0
        && memcmp(a->hx, b->hx, NX * (NY - 1) * sizeof(double)) == 0
        && memcmp(a->hy, b->hy, (NX - 1) * NY * sizeof(double)) == 0;
}

int main(void) {
    static const size_t counts[] = {2, 3, 4, 5, 7, 8, 8, 8};
    Result ref, cur;
    size_t i;
    int rep;
    int mismatches = 0;
    int runs = 0;

    TEST_START("determinismo (T hilos == 1 hilo, memcmp)");

    CHECK(run_sim(1, &ref) == 0);

    for (i = 0; i < sizeof(counts) / sizeof(counts[0]); i++) {
        for (rep = 0; rep < REPS; rep++) {
            CHECK(run_sim(counts[i], &cur) == 0);
            runs++;
            if (!identical(&ref, &cur)) {
                mismatches++;
            }
            free_result(&cur);
        }
    }
    printf("  %d de %d ejecuciones difieren del resultado con 1 hilo\n", mismatches, runs);
    CHECK(mismatches == 0);

    free_result(&ref);
    TEST_END();
}