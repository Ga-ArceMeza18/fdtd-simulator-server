#define _POSIX_C_SOURCE 200809L

#include "net_util.h"

#include <errno.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

static int connect_to(const char *host, const char *port) {
    struct addrinfo hints;
    struct addrinfo *list;
    struct addrinfo *item;
    int fd = -1;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    if (getaddrinfo(host, port, &hints, &list) != 0) {
        return -1;
    }

    for (item = list; item != NULL; item = item->ai_next) {
        fd = socket(item->ai_family, item->ai_socktype, item->ai_protocol);
        if (fd < 0) {
            continue;
        }
        if (connect(fd, item->ai_addr, item->ai_addrlen) == 0) {
            break;
        }
        close(fd);
        fd = -1;
    }

    freeaddrinfo(list);
    return fd;
}

int main(int argc, char *argv[]) {
    char command[1024];
    size_t len = 0;
    int fd;
    int i;
    ssize_t n;
    char buffer[1024];

    if (argc < 4) {
        fprintf(stderr, "uso: %s <host> <puerto> <comando> [argumentos...]\n",
            argv[0]);
        return EXIT_FAILURE;
    }

    // Une el comando y sus argumentos en una sola línea.
    for (i = 3; i < argc; i++) {
        size_t part = strlen(argv[i]);

        if (len + part + 2 > sizeof(command)) {
            fprintf(stderr, "comando demasiado largo\n");
            return EXIT_FAILURE;
        }
        if (i > 3) {
            command[len++] = ' ';
        }
        memcpy(command + len, argv[i], part);
        len += part;
    }
    command[len++] = '\n';

    fd = connect_to(argv[1], argv[2]);
    if (fd < 0) {
        fprintf(stderr, "no se pudo conectar a %s:%s\n", argv[1], argv[2]);
        return EXIT_FAILURE;
    }

    if (nu_write_all(fd, command, len) != 0) {
        close(fd);
        return EXIT_FAILURE;
    }

    // El servidor responde una línea y cierra la conexión.
    while (1) {
        n = read(fd, buffer, sizeof(buffer));
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            break;
        }
        fwrite(buffer, 1, (size_t)n, stdout);
    }

    close(fd);
    return (n < 0) ? EXIT_FAILURE : EXIT_SUCCESS;
}