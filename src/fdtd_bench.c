#include "fdtd_solver.h"

#include <time.h>
#include <stdlib.h>
#include <stdio.h>

int main(int argc, char *argv[]) {
    // 1. Leer nx, ny, pasos, hilos y k.
    if (argc != 6) {
        fprintf(stderr, "Uso: %s <nx> <ny> <pasos> <hilos> <k>\n", argv[0]);
        return EXIT_FAILURE;
    }

    int nx = atoi(argv[1]);
    int ny = atoi(argv[2]);
    int pasos = atoi(argv[3]);
    int hilos = atoi(argv[4]);
    double k = atof(argv[5]);

    if (nx <= 0 || ny <= 0 || pasos <= 0 || hilos <= 0 || k <= 0.0) {
        fprintf(stderr, "Todos los parámetros deben ser enteros positivos.\n");
        return EXIT_FAILURE;
    }

    // 2. Inicializar el solver.
    struct FDTD fdtd;
    if (fdtd_init(&fdtd, nx, ny, pasos, hilos, 0, 0) != 0) {
        fprintf(stderr, "Error al inicializar el solver FDTD.\n");
        return EXIT_FAILURE;
    }

    // 3. Medir los tiempos de ejecución.


    // 4. Ejecutar la simulación.


    // 5. Imprimir los resultados CSV.


    // 6. Liberar los recursos.
    fdtd_destroy(&fdtd);
    
    return 0;
}
