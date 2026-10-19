# Servidor de Simulación FDTD

## 1. Integrantes
- Gabriel Arce - C5C632
- Steve Puerto - C4I552
- Bryan Jimenez - C4G330

## 2. Descripción Breve

Proyecto de simulación FDTD con un servidor TCP concurrente que permite enviar trabajos, consultar su estado, observar valores publicados durante la simulación y recuperar resultados. Incluye un cliente, pruebas y herramientas de benchmark.

## 3. Requisitos

- Linux o un entorno compatible con POSIX.
- GCC con soporte para C17.
- GNU Make.
- Python 3 para generar gráficos.

## 4. Compilación

Desde la raíz del proyecto, ejecutar:

```bash
make
```

Esto compila:
- `fdtd_server`: servidor TCP.
- `fdtd_client`: cliente TCP.
- `fdtd_bench`: herramienta de benchmark.

Para compilar las versiones sin barrera:

```bash
make sinbarrera
```

## 5. Ejecución del servidor

Formato:

```bash
./fdtd_server <puerto> <sillas> <buffer> [directorio]
```

Parámetros:
- `puerto`: puerto TCP entre 1 y 65535.
- `sillas`: capacidad positiva de la cola de trabajos.
- `buffer`: capacidad positiva del buffer de snapshots.
- `directorio`: directorio donde se guardan los snapshots; por defecto, el directorio actual.

Ejemplo:

```bash
mkdir -p snapshots
./fdtd_server 8080 8 4 snapshots
```

El servidor escucha en el puerto `8080`, con 8 sillas para trabajos pendientes y un buffer de 4 snapshots.

## 6. Uso del cliente

Formato:

```bash
./fdtd_client <host> <puerto> <comando> [argumentos...]
```

Cada comando utiliza una conexión TCP independiente.

### Enviar un trabajo: `SUBMIT`

Formato:

```text
SUBMIT nx ny pasos hilos k sx sy
```

Ejemplo:

```bash
./fdtd_client 127.0.0.1 8080 SUBMIT 256 256 200 4 10 128 128
```

Parámetros:
- `nx`, `ny`: dimensiones de la simulación.
- `pasos`: cantidad de pasos de simulación.
- `hilos`: cantidad de hilos de cálculo.
- `k`: frecuencia de publicación de snapshots; `0` desactiva las publicaciones.
- `sx`, `sy`: coordenadas donde la onda de entrada ingresa a la simulación.

Una respuesta `OK <id>` indica que el trabajo fue aceptado. `BUSY` indica que no pudo admitirse; `ERROR` indica parámetros inválidos u otro error.

### Códigos de error

Los errores siguen el formato `ERROR <código> <motivo>`, con códigos de respuesta de FTP. El primer dígito indica si conviene reintentar: `4xx` es un error temporal y `5xx` uno permanente.

Ejemplos:
| `421` | El servidor está cerrando o no tiene recursos |
| `450` | El trabajo todavía no termina; reintentar después |
| `451` | Error interno al procesar la solicitud |
| `500` | Línea vacía, demasiado larga o comando desconocido |
| `501` | Argumentos o parámetros inválidos |
| `550` | El trabajo no existe, falló o fue cancelado |

### Consultar el estado: `STATUS`

```bash
./fdtd_client 127.0.0.1 8080 STATUS 1
```

El argumento es el identificador del trabajo. Las respuestas pueden indicar que está en cola (`QUEUED`), ejecutándose (`RUNNING`) o terminado (`DONE`).

### Consultar un valor: `PEEK`

```bash
./fdtd_client 127.0.0.1 8080 PEEK 1 128 128
```

Consulta el último valor publicado para el trabajo y las coordenadas indicadas. Se necesita que exista un snapshot publicado para poder obtener un valor.

### Recuperar el resultado: `RESULT`

```bash
./fdtd_client 127.0.0.1 8080 RESULT 1
```

Consulta el resultado de un trabajo terminado. La respuesta incluye la suma de valores absolutos de `Ez` y la ruta del último snapshot disponible. Si no hay una ruta de snapshot, se devuelve `-`.

### Apagar el servidor: `SHUTDOWN`

```bash
./fdtd_client 127.0.0.1 8080 SHUTDOWN
```

Solicita el cierre del servidor. Los trabajos pendientes se cancelan y el servidor espera a que finalice el trabajo activo y se vacíen los snapshots antes de terminar.

## 7. Pruebas

Compilar y ejecutar las pruebas disponibles:

```bash
make test
```

El objetivo compila los ejecutables necesarios, ejecuta las pruebas `tests/test_*.c` disponibles y comprueba la prueba asociada a la versión sin barrera. Si existe `tests/test_protocol.sh`, también ejecuta las pruebas del protocolo.

## 8. Benchmark

Ejecutar el benchmark predeterminado:

```bash
make bench
```

Para una ejecución rápida de prueba:

```bash
make bench-quick
```

También puede ejecutarse directamente:

```bash
./fdtd_bench 512 512 200 2 0
```

El benchmark permite medir el rendimiento de la simulación con diferentes dimensiones, cantidades de pasos, hilos y configuraciones de publicación. Para los resultados finales, utilizar las configuraciones y repeticiones definidas en el protocolo experimental del proyecto.

## 9. Gráficos y limpieza

Generar los gráficos de benchmark:

```bash
make plots
```

Eliminar los ejecutables y archivos de compilación:

```bash
make clean
```

## 10. Flujo de uso recomendado

1. Compilar el proyecto con `make`.
2. Ejecutar las pruebas con `make test`.
3. Iniciar el servidor indicando el puerto, las sillas, el tamaño del buffer y el directorio de snapshots.
4. Enviar un trabajo con `SUBMIT` y guardar el identificador devuelto.
5. Consultar su progreso con `STATUS` y, si se publican snapshots, consultar valores con `PEEK`.
6. Cuando termine, recuperar el resultado con `RESULT`.
7. Ejecutar los benchmarks y generar los gráficos necesarios para el informe.
8. Apagar el servidor mediante `SHUTDOWN`.
