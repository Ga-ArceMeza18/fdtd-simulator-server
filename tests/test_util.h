#ifndef TEST_UTIL_H
#define TEST_UTIL_H

// _POSIX_C_SOURCE 200809L antes de incluir cualquier encabezado.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int g_failures = 0;

#define CHECK(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "  FALLA %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        g_failures++; \
    } \
} while (0)

// Un bloqueo mutuo no debe colgar la suite: el proceso muere por SIGALRM.
#define TEST_START(name) do { printf("[%s]\n", name); fflush(stdout); alarm(120); } while (0)

#define TEST_END() do { \
    if (g_failures == 0) { printf("  OK\n"); } \
    else { printf("  %d verificacion(es) fallaron\n", g_failures); } \
    return (g_failures == 0) ? EXIT_SUCCESS : EXIT_FAILURE; \
} while (0)

static inline double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static inline void sleep_us(long us) {
    struct timespec ts;
    ts.tv_sec = us / 1000000;
    ts.tv_nsec = (us % 1000000) * 1000;
    nanosleep(&ts, NULL);
}

static inline void sleep_ms(long ms) {
    sleep_us(ms * 1000);
}

#endif