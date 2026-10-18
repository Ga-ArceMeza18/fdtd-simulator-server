#ifndef STATUS_H
#define STATUS_H

#include <stddef.h>

// Estado publicado de la simulación FDTD
// Estructura opaca para representar el estado de la simulación FDTD
typedef struct Status Status;

// Inicializa un objeto Status con las dimensiones especificadas
// Status **status permite que la función cree la estructura Status y devuelva un puntero a ella. 
// Esto es útil cuando se desea inicializar un objeto Status desde fuera de la función y mantener su referencia.

int status_init(Status **status, size_t nx, size_t ny);

// Publica una nueva versión de Ez
// const double *ez es un puntero a un arreglo de valores Ez que se publicarán en el estado sin modificar el arreglo original en el solver.
// Esto permite que el solver mantenga su propio arreglo de Ez mientras que el estado mantiene una copia de los valores publicados.

int status_publish(Status *status, size_t step, const double *ez);

// Consulta la versión más reciente de Ez
// ez_value es un puntero a un arreglo donde se almacenarán los valores de Ez consultados.
// Esto es útil para obtener los valores de Ez en un momento específico sin modificar el estado interno del objeto Status.

int status_consult(Status *status, size_t *step, size_t i, size_t j, double *ez_value);

// Progreso de la simulación (paso actual). Es barato: no copia Ez. Lo escribe
// el hilo 0 en cada paso y lo lee STATUS; usa el mismo candado lectores-escritor.
int status_set_step(Status *status, size_t step);
int status_get_step(Status *status, size_t *step);

// Libera los recursos asociados con el objeto Status

void status_destroy(Status *status);


#endif // STATUS_H