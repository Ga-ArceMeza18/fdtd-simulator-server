#ifndef FDTD_H
#define FDTD_H

#include <stddef.h>

// Campo Eletromagnético

/*
* Ez: nx * ny
* Hx: nx * (ny - 1)
* Hy: (nx - 1) * ny
*
* Los campos se almacenan en memoria contigua de manera que se pueda acceder
* como un arreglo de una dimensión.
*/
struct FDTD {
    size_t nx;
    size_t ny;
    size_t steps;

    size_t sx;
    size_t sy;

    size_t num_threads;

    double *ez;
    double *hx;
    double *hy;
};


// Inicializar los campos en cero y reservar memoria para los campos.
// Retorna 0 si la inicialización fue exitosa, -1 si hubo un error.
int fdtd_init(struct FDTD *fdtd, size_t nx, size_t ny, size_t steps,
    size_t num_threads, size_t sx, size_t sy);
    
// Ejecutar la simulación FDTD.
// Pendiente: agregar la barrera reutilizable
int fdtd_run(struct FDTD *fdtd);

// Calcular la suma de los valores de Ez.
double fdtd_sum_ez(const struct FDTD *fdtd);

// Liberar la memoria reservada para los campos.
void fdtd_destroy(struct FDTD *fdtd);

#endif // FDTD_H