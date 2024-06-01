#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <pthread.h>
#include <microhttpd.h>
#include "rest_client.h" 

#define UNIX_SOCKET_PATH "/tmp/unix_socket"
#define INET_PORT 12345
#define HTTP_PORT 8888
#define BUFFER_SIZE 1024

int unix_server_socket = -1;
int inet_server_socket = -1;

void cleanup(int signo) {
    if (unix_server_socket != -1) {
        close(unix_server_socket);
        unlink(UNIX_SOCKET_PATH);
    }
    if (inet_server_socket != -1) {
        close(inet_server_socket);
    }
    printf("Server shutting down gracefully...\n");
    exit(0);
}

void *handle_client(void *arg) {
    int client_socket = *(int *)arg;
    free(arg);
    char buffer[BUFFER_SIZE];
    int bytes_read;

    while ((bytes_read = read(client_socket, buffer, BUFFER_SIZE)) > 0) {
        buffer[bytes_read] = '\0';
        printf("Received: %s\n", buffer);

        if (strncmp(buffer, "GET /rest", 9) == 0) {
            char response[1024] = {0};
            make_rest_request(response);
            write(client_socket, response, strlen(response));
        } else {
            write(client_socket, buffer, bytes_read);
        }
    }

    close(client_socket);
    return NULL;
}

void *unix_server(void *arg) {
    int client_socket, *new_sock;
    struct sockaddr_un server_addr;

    unix_server_socket = socket(AF_UNIX, SOCK_STREAM, 0);
    if (unix_server_socket == -1) {
        perror("Socket error");
        exit(EXIT_FAILURE);
    }

    memset(&server_addr, 0, sizeof(struct sockaddr_un));
    server_addr.sun_family = AF_UNIX;
    strncpy(server_addr.sun_path, UNIX_SOCKET_PATH, sizeof(server_addr.sun_path) - 1);

    unlink(UNIX_SOCKET_PATH);

    if (bind(unix_server_socket, (struct sockaddr *)&server_addr, sizeof(struct sockaddr_un)) == -1) {
        perror("Bind error");
        exit(EXIT_FAILURE);
    }

    if (listen(unix_server_socket, 5) == -1) {
        perror("Listen error");
        exit(EXIT_FAILURE);
    }

    printf("Unix server is waiting for connections...\n");

    while ((client_socket = accept(unix_server_socket, NULL, NULL)) != -1) {
        new_sock = malloc(sizeof(int));
        *new_sock = client_socket;
        pthread_t thread;
        pthread_create(&thread, NULL, handle_client, (void *)new_sock);
        pthread_detach(thread);
    }

    close(unix_server_socket);
    unlink(UNIX_SOCKET_PATH);
    return NULL;
}

void *inet_server(void *arg) {
    int client_socket, *new_sock;
    struct sockaddr_in server_addr, client_addr;
    socklen_t client_addr_len = sizeof(client_addr);

    inet_server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (inet_server_socket == -1) {
        perror("Socket error");
        exit(EXIT_FAILURE);
    }

    int opt = 1;
    if (setsockopt(inet_server_socket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) == -1) {
        perror("Setsockopt error");
        exit(EXIT_FAILURE);
    }

    memset(&server_addr, 0, sizeof(struct sockaddr_in));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(INET_PORT);

    if (bind(inet_server_socket, (struct sockaddr *)&server_addr, sizeof(server_addr)) == -1) {
        perror("Bind error");
        exit(EXIT_FAILURE);
    }

    if (listen(inet_server_socket, 5) == -1) {
        perror("Listen error");
        exit(EXIT_FAILURE);
    }

    printf("INET server is waiting for connections...\n");

    while ((client_socket = accept(inet_server_socket, (struct sockaddr *)&client_addr, &client_addr_len)) != -1) {
        new_sock = malloc(sizeof(int));
        *new_sock = client_socket;
        pthread_t thread;
        pthread_create(&thread, NULL, handle_client, (void *)new_sock);
        pthread_detach(thread);
    }

    close(inet_server_socket);
    return NULL;
}

static int answer_to_connection(void *cls, struct MHD_Connection *connection,
                                const char *url, const char *method,
                                const char *version, const char *upload_data,
                                size_t *upload_data_size, void **con_cls) {
    struct MHD_Response *response;
    int ret;

    if (strncmp(url, "/rest", 5) == 0) {
        char rest_response[1024] = {0};
        make_rest_request(rest_response);
        response = MHD_create_response_from_buffer(strlen(rest_response), rest_response, MHD_RESPMEM_MUST_COPY);
        MHD_add_response_header(response, "Access-Control-Allow-Origin", "*");
        MHD_add_response_header(response, "Content-Type", "application/json");
        ret = MHD_queue_response(connection, MHD_HTTP_OK, response);
        MHD_destroy_response(response);
    } else {
        const char *page = "<html><body><h1>Hello, browser!</h1></body></html>";
        response = MHD_create_response_from_buffer(strlen(page), (void *)page, MHD_RESPMEM_PERSISTENT);
        MHD_add_response_header(response, "Access-Control-Allow-Origin", "*");
        MHD_add_response_header(response, "Content-Type", "text/html");
        ret = MHD_queue_response(connection, MHD_HTTP_OK, response);
        MHD_destroy_response(response);
    }

    return ret;
}

void *http_server(void *arg) {
    struct MHD_Daemon *daemon;

    daemon = MHD_start_daemon(MHD_USE_SELECT_INTERNALLY, HTTP_PORT, NULL, NULL,
                              &answer_to_connection, NULL, MHD_OPTION_END);
    if (NULL == daemon) return NULL;

    printf("HTTP server is running on port %d\n", HTTP_PORT);
    getchar(); // Wait for user input to stop the server : use CTRL + C gen

    MHD_stop_daemon(daemon);
    return NULL;
}

int main() {
    signal(SIGINT, cleanup);
    signal(SIGTERM, cleanup);

    pthread_t unix_thread, inet_thread, http_thread;

    pthread_create(&unix_thread, NULL, unix_server, NULL);
    pthread_create(&inet_thread, NULL, inet_server, NULL);
    pthread_create(&http_thread, NULL, http_server, NULL);

    pthread_join(unix_thread, NULL);
    pthread_join(inet_thread, NULL);
    pthread_join(http_thread, NULL);

    return 0;
}
