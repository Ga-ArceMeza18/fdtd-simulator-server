//
// Created by Sleyter Angulo on 9/17/26.
//

#include "../includes/net_util.h"
#include <pthread.h>
#include <semaphore.h> // se añade para usar los semáforos
#include <netinet/in.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

#define N 32
#define POISON_PILL -1

// la conexion del cliente necesito un file descriptor y un id de conexion
typedef struct {
    int fd;
    unsigned long connection_id;
} client_conn_t;

// la cola compartido del productor y el consumidor necesita algunos atributos
typedef struct {
    int index_in;
    int index_out;
    client_conn_t buffer[N];
    pthread_mutex_t mutex;
    sem_t empty; 
    sem_t full;
} queue_t;

// el consumidor necesito atributos como la cola(compartida) y la cantidad de paquetes que ha completado
typedef struct {
    queue_t *queue;
    unsigned long completed;
} consumer_args_t;


// el productor necesita una cola(compartida), el file descriptor del server, paquetes y cantidad de consumidores
typedef struct {
    queue_t *queue;
    int server_fd;
    long total_request;
    int num_consumers;
} producer_args_t;

// inicializar los valores
void queue_init(queue_t *qp) {
    qp->index_in = 0;
    qp->index_out = 0;
    pthread_mutex_init(&qp->mutex, NULL);
    sem_init(&qp->empty, 0, N);
    sem_init(&qp->full, 0, 0);

}

void push_queue(queue_t *qp, int fd, unsigned long connection_id) {
    
    sem_wait( &qp->empty ); // se quita el while que valoraba cuando la cola estaba llena
    pthread_mutex_lock(&qp->mutex); // "ocupamos" hilo

    qp->buffer[qp->index_in].fd = fd; // colocamos el file descriptor en el buffer
    qp->buffer[qp->index_in].connection_id = connection_id;
    qp->index_in = (qp->index_in + 1) % N; // aumentamos el index_in

    pthread_mutex_unlock( &qp->mutex );
    sem_post( &qp->full ); // se añade un nuevo elemento (se suma 1)
}

client_conn_t pop_queue(queue_t *qp){
    
    sem_wait( &qp->full );
    pthread_mutex_lock( &qp->mutex ); // ocupamos el hilo

    client_conn_t conn = qp->buffer[qp->index_out]; // sacamos un dato de la cola
    qp->index_out = (qp->index_out + 1) % N; // aumentamos el index_out

    pthread_mutex_unlock( &qp->mutex );
    sem_post( &qp->empty );

    return conn;
}

void *producer(void *arg) {
    producer_args_t *args = arg;
    unsigned long connection_id = 0;

    for (long i = 0; i < args->total_request; i++) {
        struct sockaddr_in client_address; // struct por defecto de netinet

        socklen_t addr_len = sizeof(client_address);

        // la función accept toma (int, struct sockaddr *, socklen_t); con estos tres parámetros ya trabaja
        int client_fd = accept(args->server_fd, (struct sockaddr *)&client_address, &addr_len);

        // si falló la conexión, lo sigue intentando
        if (client_fd < 0) {
            perror("accept");
            continue;
        }

        connection_id++;
        push_queue(args->queue, client_fd, connection_id);
    }

    // apagar los consumidores mediante el posion_pill
    for (int i = 0; i < args->num_consumers; i++) {
        push_queue(args->queue, POISON_PILL, 0);
    }

    return NULL;
}

void *consumer(void *arg){
    consumer_args_t *args = arg;

    while (1) {
        client_conn_t conn = pop_queue(args->queue);

        // si el file descriptor es la píldora envenenada, dormimos el consumidor
        if (conn.fd == POISON_PILL) {
            break;
        }

        printf("[Handling connection %lu] accepted\n", conn.connection_id);
        fflush(stdout);

        
        if (nu_drain_request(conn.fd) >= 0) {
            nu_send_response(conn.fd, conn.connection_id);
        }

        // ERROR
        if (close(conn.fd) < 0) {
            perror("close");
        }

        args->completed++;
    }

    return NULL;
}

int main(int argc, char *argv[]) {
    
    // validamos que estén los 3 argumentos que necesitamos
    if (argc != 4) {
        fprintf(stderr, "uso: %s <puerto> <num_consumers> <total_requests>\n", argv[0]);
        return EXIT_FAILURE;
    }

    // definimos variables usando los argumentos
    // atoi = de valor char a valor int
    unsigned short port = (unsigned short)atoi(argv[1]);
    int num_consumers = atoi(argv[2]);
    long total_requests = atol(argv[3]);

    // definición de la variable server_fd
    int server_fd = nu_listen(port, 64);
    
    // si sucede un error
    if (server_fd < 0) {
        return EXIT_FAILURE;
    }

    printf("listening on port %u\n", port);
    fflush(stdout);

    // creación de la cola e inicialización
    queue_t queue;
    queue_init(&queue);

    producer_args_t producer_args = {
        .queue = &queue,
        .server_fd = server_fd,
        .total_request = total_requests,
        .num_consumers = num_consumers
    };

    pthread_t producer_thread;
    pthread_create(&producer_thread, NULL, producer, &producer_args);


    // reservamos memoria de manera dinámica
    pthread_t *consumer_threads = calloc(num_consumers, sizeof(pthread_t));
    consumer_args_t *consumer_args = calloc(num_consumers, sizeof(consumer_args_t));

    for (int i = 0; i < num_consumers; i++) {
        consumer_args[i].queue = &queue;
        consumer_args[i].completed = 0;
        pthread_create(&consumer_threads[i], NULL, consumer, &consumer_args[i]);
    }

    // esperamos al productor
    pthread_join(producer_thread, NULL);

    // esperamos a los consumidores y sumamos los resultados a lo requests completados
    unsigned long total_completed = 0;
    for (int i = 0; i < num_consumers; i++) {
        pthread_join(consumer_threads[i], NULL);
        total_completed += consumer_args[i].completed;
    }

    printf("total procesado: %lu\n", total_completed);

    free(consumer_threads);
    free(consumer_args);
    close(server_fd);

    sem_destroy(&queue.empty);
    sem_destroy(&queue.full);
    pthread_mutex_destroy(&queue.mutex);

    return 0;
}