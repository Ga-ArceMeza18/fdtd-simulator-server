#define _POSIX_C_SOURCE 200809L

#include "barbershop.h"
#include "net_util.h"
#include "snapshot_buffer.h"

#include <ctype.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define LINE_MAX_LEN 256
#define MAX_TOKENS 9
#define RECV_TIMEOUT_SECONDS 30

// Límites propios del servidor para que una solicitud absurda no lo tumbe.
#define MAX_DIM 8192
#define MAX_THREADS 256
#define MAX_STEPS 10000000UL

struct Server;

// Una conexión activa. Todas viven en una lista doble para poder despertar
// a los manejadores bloqueados leyendo cuando llega SHUTDOWN.
typedef struct Conn {
    int fd;
    struct Server *server;
    struct Conn *prev;
    struct Conn *next;
} Conn;

typedef struct Server {
    int listen_fd;
    Barbershop *shop;
    SnapshotBuffer *snapshots;

    pthread_mutex_t mutex;    // Protege todo lo de abajo
    pthread_cond_t changed;   // Cambió active o shutdown_done
    Conn *conns;              // Lista de conexiones activas
    size_t active;            // Manejadores que todavía no terminan
    int stopping;             // 1 desde que llegó el primer SHUTDOWN
    int shutdown_done;        // 1 cuando el apagado del trabajo terminó
} Server;

// ---------------------------------------------------------------- utilidades

// Envía una línea de respuesta (se le agrega el salto de línea).
static void reply(int fd, const char *fmt, ...) {
    char buffer[512];
    va_list args;
    int n;

    va_start(args, fmt);
    n = vsnprintf(buffer, sizeof(buffer) - 1, fmt, args);
    va_end(args);

    if (n < 0) {
        return;
    }
    if ((size_t)n > sizeof(buffer) - 2) {
        n = (int)sizeof(buffer) - 2;
    }
    buffer[n++] = '\n';

    nu_write_all(fd, buffer, (size_t)n);
}

// Convierte un token a size_t. Solo acepta dígitos decimales (nada de signos,
// espacios ni basura al final) y rechaza los desbordamientos.
static int parse_size(const char *text, size_t *out) {
    char *end;
    unsigned long long value;

    if (text == NULL || !isdigit((unsigned char)text[0])) {
        return -1;
    }

    errno = 0;
    value = strtoull(text, &end, 10);
    if (errno != 0 || *end != '\0' || value > SIZE_MAX) {
        return -1;
    }

    *out = (size_t)value;
    return 0;
}

// ------------------------------------------------------------------ comandos

static void cmd_submit(Server *srv, int fd, int argc, char **argv) {
    size_t v[7]; // nx ny pasos hilos k sx sy
    JobParams params;
    unsigned long id = 0;
    int i;

    if (argc != 8) {
        reply(fd, "ERROR 501 uso: SUBMIT nx ny pasos hilos k sx sy");
        return;
    }
    for (i = 0; i < 7; i++) {
        if (parse_size(argv[i + 1], &v[i]) != 0) {
            reply(fd, "ERROR 501 parametro no numerico: %s", argv[i + 1]);
            return;
        }
    }

    params.nx = v[0];
    params.ny = v[1];
    params.steps = v[2];
    params.threads = v[3];
    params.k = v[4];
    params.sx = v[5];
    params.sy = v[6];

    if (params.nx > MAX_DIM || params.ny > MAX_DIM) {
        reply(fd, "ERROR 501 nx y ny deben ser <= %d", MAX_DIM);
        return;
    }
    if (params.steps > MAX_STEPS) {
        reply(fd, "ERROR 501 pasos debe ser <= %lu", MAX_STEPS);
        return;
    }
    if (params.threads > MAX_THREADS) {
        reply(fd, "ERROR 501 hilos debe ser <= %d", MAX_THREADS);
        return;
    }

    switch (barbershop_submit(srv->shop, &params, &id)) {
    case BARBERSHOP_OK:
        reply(fd, "OK %lu", id);
        break;
    case BARBERSHOP_BUSY:
        reply(fd, "BUSY");
        break;
    case BARBERSHOP_INVALID:
        reply(fd, "ERROR 501 parametros invalidos (nx,ny>=3; pasos>0; "
            "1<=hilos<=nx-2; 0<=k<=pasos; 1<=sx<=nx-2; 1<=sy<=ny-2)");
        break;
    case BARBERSHOP_CLOSED:
        reply(fd, "ERROR 421 servidor cerrando");
        break;
    default:
        reply(fd, "ERROR 451 error interno");
        break;
    }
}

static void cmd_status(Server *srv, int fd, int argc, char **argv) {
    size_t id;
    JobInfo info;

    if (argc != 2 || parse_size(argv[1], &id) != 0) {
        reply(fd, "ERROR 501 uso: STATUS id");
        return;
    }
    if (barbershop_query(srv->shop, id, &info) != 0) {
        reply(fd, "ERROR 550 id desconocido");
        return;
    }

    switch (info.state) {
    case JOB_QUEUED:
        reply(fd, "QUEUED %zu", info.position);
        break;
    case JOB_RUNNING:
        reply(fd, "RUNNING %zu/%zu", info.step, info.steps);
        break;
    case JOB_DONE:
        reply(fd, "DONE %.6f", info.seconds);
        break;
    default:
        reply(fd, "ERROR 550 trabajo fallido o cancelado");
        break;
    }
}

static void cmd_peek(Server *srv, int fd, int argc, char **argv) {
    size_t id;
    size_t i;
    size_t j;
    size_t step;
    double ez;
    JobInfo info;

    if (argc != 4 || parse_size(argv[1], &id) != 0
        || parse_size(argv[2], &i) != 0 || parse_size(argv[3], &j) != 0) {
        reply(fd, "ERROR 501 uso: PEEK id i j");
        return;
    }
    if (barbershop_query(srv->shop, id, &info) != 0) {
        reply(fd, "ERROR 550 id desconocido");
        return;
    }
    if (barbershop_peek(srv->shop, id, i, j, &step, &ez) != 0) {
        reply(fd, "ERROR 501 indices fuera de la malla (0<=i<nx, 0<=j<ny)");
        return;
    }

    reply(fd, "VALUE %zu %.10e", step, ez);
}

static void cmd_result(Server *srv, int fd, int argc, char **argv) {
    size_t id;
    JobInfo info;
    char path[512];

    if (argc != 2 || parse_size(argv[1], &id) != 0) {
        reply(fd, "ERROR 501 uso: RESULT id");
        return;
    }
    if (barbershop_query(srv->shop, id, &info) != 0) {
        reply(fd, "ERROR 550 id desconocido");
        return;
    }

    if (info.state == JOB_QUEUED || info.state == JOB_RUNNING) {
        reply(fd, "ERROR 450 el trabajo no ha terminado");
        return;
    }
    if (info.state == JOB_ERROR) {
        reply(fd, "ERROR 550 trabajo fallido o cancelado");
        return;
    }

    // <archivo>: la última instantánea del trabajo ("-" si no se pidieron).
    if (info.last_snapshot == 0
        || snapbuf_path(srv->snapshots, id, info.last_snapshot, path,
            sizeof(path)) != 0) {
        strcpy(path, "-");
    }

    reply(fd, "RESULT %.10e %s", info.sum_abs_ez, path);
}

// SHUTDOWN: el primer manejador que llega hace el trabajo; si llegan otros
// SHUTDOWN, esperan a que termine.
static void server_shutdown(Server *srv) {
    Conn *conn;

    pthread_mutex_lock(&srv->mutex);

    if (srv->stopping) {
        while (!srv->shutdown_done) {
            pthread_cond_wait(&srv->changed, &srv->mutex);
        }
        pthread_mutex_unlock(&srv->mutex);
        return;
    }
    srv->stopping = 1;

    // Despierta al hilo principal, que está bloqueado en accept().
    shutdown(srv->listen_fd, SHUT_RDWR);

    // Despierta a los manejadores bloqueados esperando una línea de un cliente
    // que no escribe. Lo que ya llegó a su búfer todavía se puede leer.
    for (conn = srv->conns; conn != NULL; conn = conn->next) {
        shutdown(conn->fd, SHUT_RD);
    }
    pthread_mutex_unlock(&srv->mutex);

    barbershop_shutdown(srv->shop);  // cancela la cola y espera al trabajo actual
    snapbuf_flush(srv->snapshots);   // todo lo depositado queda en disco

    pthread_mutex_lock(&srv->mutex);
    srv->shutdown_done = 1;
    pthread_cond_broadcast(&srv->changed);
    pthread_mutex_unlock(&srv->mutex);
}

static void process_line(Server *srv, int fd, char *line) {
    char *argv[MAX_TOKENS];
    char *save = NULL;
    char *token;
    int argc = 0;

    for (token = strtok_r(line, " \t", &save); token != NULL;
         token = strtok_r(NULL, " \t", &save)) {
        if (argc == MAX_TOKENS) {
            reply(fd, "ERROR 501 demasiados argumentos");
            return;
        }
        argv[argc++] = token;
    }

    if (argc == 0) {
        reply(fd, "ERROR 500 linea vacia");
    } else if (strcmp(argv[0], "SUBMIT") == 0) {
        cmd_submit(srv, fd, argc, argv);
    } else if (strcmp(argv[0], "STATUS") == 0) {
        cmd_status(srv, fd, argc, argv);
    } else if (strcmp(argv[0], "PEEK") == 0) {
        cmd_peek(srv, fd, argc, argv);
    } else if (strcmp(argv[0], "RESULT") == 0) {
        cmd_result(srv, fd, argc, argv);
    } else if (strcmp(argv[0], "SHUTDOWN") == 0) {
        if (argc != 1) {
            reply(fd, "ERROR 501 uso: SHUTDOWN");
        } else {
            server_shutdown(srv);
            reply(fd, "BYE");
        }
    } else {
        reply(fd, "ERROR 500 comando desconocido");
    }
}

// ----------------------------------------------------------------- manejador

static void *handler_thread(void *arg) {
    Conn *conn = arg;
    Server *srv = conn->server;
    char line[LINE_MAX_LEN];
    ssize_t n;

    n = nu_read_line(conn->fd, line, sizeof(line));
    if (n == -2) {
        reply(conn->fd, "ERROR 500 linea demasiado larga");

        // Quedan bytes sin leer. Si se cerrara ya, TCP enviaría un RST que
        // puede borrar la respuesta antes de que el cliente la lea. Se cierra
        // la escritura y se descarta lo que siga llegando, con un tope.
        shutdown(conn->fd, SHUT_WR);
        {
            char discard[1024];
            size_t total = 0;
            ssize_t got;

            while (total < (1u << 20)
                && (got = read(conn->fd, discard, sizeof(discard))) > 0) {
                total += (size_t)got;
            }
        }
    } else if (n >= 0) {
        process_line(srv, conn->fd, line);
    }
    // n == -1: error de lectura o tiempo de espera agotado; solo se cierra.

    // Se saca de la lista ANTES de cerrar el descriptor: así SHUTDOWN nunca
    // llama shutdown() sobre un descriptor que ya se cerró (y pudo reutilizarse).
    pthread_mutex_lock(&srv->mutex);
    if (conn->prev != NULL) {
        conn->prev->next = conn->next;
    } else {
        srv->conns = conn->next;
    }
    if (conn->next != NULL) {
        conn->next->prev = conn->prev;
    }
    pthread_mutex_unlock(&srv->mutex);

    close(conn->fd);
    free(conn);

    pthread_mutex_lock(&srv->mutex);
    srv->active--;
    pthread_cond_broadcast(&srv->changed);
    pthread_mutex_unlock(&srv->mutex);

    return NULL;
}

// Registra la conexión y lanza su manejador (desacoplado: nadie le hace join,
// el hilo principal espera a que active llegue a cero al apagar).
static int spawn_handler(Server *srv, int fd) {
    Conn *conn;
    pthread_attr_t attr;
    pthread_t thread;
    struct timeval timeout;
    int rc;

    conn = calloc(1, sizeof(*conn));
    if (conn == NULL) {
        return -1;
    }
    conn->fd = fd;
    conn->server = srv;

    // Un cliente que conecta y no escribe no puede retener el hilo para siempre.
    timeout.tv_sec = RECV_TIMEOUT_SECONDS;
    timeout.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    pthread_mutex_lock(&srv->mutex);
    conn->next = srv->conns;
    if (srv->conns != NULL) {
        srv->conns->prev = conn;
    }
    srv->conns = conn;
    srv->active++;
    if (srv->stopping) {
        // Llegó justo durante el apagado: no se le deja esperar una línea.
        shutdown(fd, SHUT_RD);
    }
    pthread_mutex_unlock(&srv->mutex);

    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    rc = pthread_create(&thread, &attr, handler_thread, conn);
    pthread_attr_destroy(&attr);

    if (rc != 0) {
        pthread_mutex_lock(&srv->mutex);
        if (conn->prev != NULL) {
            conn->prev->next = conn->next;
        } else {
            srv->conns = conn->next;
        }
        if (conn->next != NULL) {
            conn->next->prev = conn->prev;
        }
        srv->active--;
        pthread_cond_broadcast(&srv->changed);
        pthread_mutex_unlock(&srv->mutex);

        free(conn);
        return -1;
    }

    return 0;
}

// ---------------------------------------------------------------------- main

int main(int argc, char *argv[]) {
    Server srv;
    size_t port;
    size_t chairs;
    size_t buffer;
    const char *dir;
    int mutex_ok = 0;
    int cond_ok = 0;
    int status = EXIT_FAILURE;

    if (argc < 4 || argc > 5) {
        fprintf(stderr, "uso: %s <puerto> <sillas C> <buffer B> [directorio]\n",
            argv[0]);
        return EXIT_FAILURE;
    }
    if (parse_size(argv[1], &port) != 0 || port == 0 || port > 65535
        || parse_size(argv[2], &chairs) != 0 || chairs == 0
        || parse_size(argv[3], &buffer) != 0 || buffer == 0) {
        fprintf(stderr, "puerto (1-65535), sillas y buffer deben ser enteros positivos\n");
        return EXIT_FAILURE;
    }
    dir = (argc == 5) ? argv[4] : ".";

    // Escribir en un socket cerrado por el cliente no debe matar al servidor.
    signal(SIGPIPE, SIG_IGN);

    memset(&srv, 0, sizeof(srv));
    srv.listen_fd = -1;

    if (pthread_mutex_init(&srv.mutex, NULL) != 0) {
        return EXIT_FAILURE;
    }
    mutex_ok = 1;
    if (pthread_cond_init(&srv.changed, NULL) != 0) {
        goto cleanup;
    }
    cond_ok = 1;

    if (snapbuf_init(&srv.snapshots, buffer, dir) != 0) {
        fprintf(stderr, "no se pudo crear el bufer de instantaneas en '%s'\n", dir);
        goto cleanup;
    }
    if (barbershop_init(&srv.shop, chairs) != 0) {
        fprintf(stderr, "no se pudo crear la barberia\n");
        goto cleanup;
    }
    barbershop_set_snapshots(srv.shop, srv.snapshots);

    srv.listen_fd = nu_listen((unsigned short)port, 64);
    if (srv.listen_fd < 0) {
        goto cleanup;
    }

    printf("listening on port %zu (sillas=%zu, buffer=%zu, dir=%s)\n",
        port, chairs, buffer, dir);
    fflush(stdout);

    // Ciclo de accept: termina cuando SHUTDOWN hace shutdown() sobre el socket.
    while (1) {
        int fd = accept(srv.listen_fd, NULL, NULL);

        if (fd < 0) {
            int stopping;

            pthread_mutex_lock(&srv.mutex);
            stopping = srv.stopping;
            pthread_mutex_unlock(&srv.mutex);

            if (stopping) {
                break;
            }
            if (errno == EINTR || errno == ECONNABORTED) {
                continue;
            }
            if (errno == EMFILE || errno == ENFILE || errno == ENOBUFS
                || errno == ENOMEM) {
                struct timespec pause = {0, 10 * 1000 * 1000}; // 10 ms
                perror("accept");
                nanosleep(&pause, NULL);
                continue;
            }
            perror("accept");
            break;
        }

        if (spawn_handler(&srv, fd) != 0) {
            reply(fd, "ERROR 421 servidor sin recursos");
            close(fd);
        }
    }

    // Apagado ordenado: se espera a todos los manejadores (incluido el que
    // atendió SHUTDOWN) antes de liberar nada de lo que ellos usan.
    pthread_mutex_lock(&srv.mutex);
    while (srv.active > 0) {
        pthread_cond_wait(&srv.changed, &srv.mutex);
    }
    pthread_mutex_unlock(&srv.mutex);

    status = EXIT_SUCCESS;

cleanup:
    if (srv.shop != NULL) {
        barbershop_destroy(srv.shop);   // hace join del barbero si no se había hecho
    }
    if (srv.snapshots != NULL) {
        snapbuf_destroy(srv.snapshots); // el escritor vacía el búfer y termina
    }
    if (srv.listen_fd >= 0) {
        close(srv.listen_fd);
    }
    if (cond_ok) {
        pthread_cond_destroy(&srv.changed);
    }
    if (mutex_ok) {
        pthread_mutex_destroy(&srv.mutex);
    }

    if (status == EXIT_SUCCESS) {
        printf("servidor apagado\n");
    }
    return status;
}