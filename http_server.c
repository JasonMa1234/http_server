// gcc -o http_server2 http_server2.c -pthread -std=c11
// Run: ./http_server2 <port>
// Test: curl -v -X GET http://localhost:<port>

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <errno.h>
#include <stdatomic.h>
#include <time.h>
#include <sys/resource.h>

#define PORT 8080
#define MAX_EVENTS 64
#define MAX_WORKERS 4
#define BUFFER_SIZE 8192

typedef struct {
    int epoll_fd;
    int thread_id;
} worker_t;

int server_socket;
worker_t workers[MAX_WORKERS];
pthread_t worker_threads[MAX_WORKERS];
int next_worker = 0; // Round-robin index

#define HTTP_ERROR_RESP_NUM 6

const char *err_response[HTTP_ERROR_RESP_NUM] = {
    "HTTP/1.1 400 Bad Request\r\n"
    "Content-Type: application/json\r\n"
    "Content-Length: 52\r\n"
    "\r\n"
    "{\r\n"
    "  \"error\": \"Bad Request\",\r\n"
    "  \"message\": \"Invalid input\"\r\n"
    "}",

    "HTTP/1.1 401 Unauthorized\r\n"
    "WWW-Authenticate: Bearer realm=\"example\"\r\n"
    "Content-Type: application/json\r\n"
    "Content-Length: 55\r\n"
    "\r\n"
    "{\r\n"
    "  \"error\": \"Unauthorized\",\r\n"
    "  \"message\": \"Invalid token\"\r\n"
    "}",

    "HTTP/1.1 403 Forbidden\r\n"
    "Content-Type: application/json\r\n"
    "Content-Length: 54\r\n"
    "\r\n"
    "{\r\n"
    "  \"error\": \"Forbidden\",\r\n"
    "  \"message\": \"Access denied\"\r\n"
    "}",

    "HTTP/1.1 404 Not Found\r\n"
    "Content-Type: application/json\r\n"
    "Content-Length: 56\r\n"
    "\r\n"
    "{\r\n"
    "  \"error\": \"Not Found\",\r\n"
    "  \"message\": \"Resource not found\"\r\n"
    "}",

    "HTTP/1.1 500 Internal Server Error\r\n"
    "Content-Type: application/json\r\n"
    "Content-Length: 63\r\n"
    "\r\n"
    "{\r\n"
    "  \"error\": \"Internal Server Error\",\r\n"
    "  \"message\": \"Something went wrong\"\r\n"
    "}",

    "HTTP/1.1 503 Service Unavailable\r\n"
    "Retry-After: 3600\r\n"
    "Content-Type: application/json\r\n"
    "Content-Length: 65\r\n"
    "\r\n"
    "{\r\n"
    "  \"error\": \"Service Unavailable\",\r\n"
    "  \"message\": \"Server is overloaded\"\r\n"
    "}"
};


atomic_int request_count = 0;       // Atomic counter for requests
atomic_int active_connections = 0;  // Atomic counter for requests
atomic_int debug_mode = 0;          // Debug mode (0 = off, 1 = on)
int is_bg = 0;
int delay = 0;
float errate = 0;
int send_err_cycle = 0;
int err_index_g = 0;

void increase_fd_limit() {
    struct rlimit limit;
    getrlimit(RLIMIT_NOFILE, &limit);
    printf("Current limit: %ld\n", limit.rlim_cur);

    limit.rlim_cur = 100000;  // Increase soft limit
    limit.rlim_max = 100000;  // Increase hard limit
    if (setrlimit(RLIMIT_NOFILE, &limit) != 0) {
        perror("setrlimit failed");
    } else {
        //printf("New limit: %ld\n", limit.rlim_cur);
    }
}

// Print timestamped stats every 10 seconds
void print_timestamp() {
    static int last_cnt = 0;
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    char time_str[20];
    strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", tm_info);
    if (last_cnt == 0)
        printf("[%s] Requests received[%d]: %d\n", time_str, active_connections, request_count);
    else
        printf("[%s] Requests received[%d]: %d, Msg per Second: %.2f\n", time_str, active_connections, request_count, ((double)(request_count - last_cnt))/10);
    last_cnt = request_count;
}

// Stats logging thread
void *stats_thread(void *arg) {
    (void)arg;
    while (1) {
        sleep(10);
        print_timestamp();
    }
    return NULL;
}

// Debug mode input thread
void *debug_toggle_thread(void *arg) {
    if (is_bg)
        return NULL;

    while (1) {
        char input;
        if (read(STDIN_FILENO, &input, 1) > 0) {
            if (input == 'd' || input == 'D') {
                debug_mode = !debug_mode;
                printf("\n[INFO] Debug mode %s\n", debug_mode ? "ENABLED" : "DISABLED");
            }
        }
    }
    return NULL;
}

// Set socket to non-blocking mode
void set_nonblocking(int sock) {
    int flags = fcntl(sock, F_GETFL, 0);
    fcntl(sock, F_SETFL, flags | O_NONBLOCK);
}

// Worker thread function
void *worker_function(void *arg) {
    worker_t *worker = (worker_t *)arg;
    struct epoll_event events[MAX_EVENTS];
    int resp_cnt = 0;
    int err_idx = 0;

    printf("[Worker %d] Started\n", worker->thread_id);

    while (1) {
        int num_events = epoll_wait(worker->epoll_fd, events, MAX_EVENTS, -1);
        if (num_events < 0) {
            perror("epoll_wait failed");
            continue;
        }

        for (int i = 0; i < num_events; i++) {
            int client_socket = events[i].data.fd;
            char buffer[BUFFER_SIZE];

            ssize_t bytes_received = recv(client_socket, buffer, BUFFER_SIZE - 1, 0);
            if (bytes_received < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    // No data available right now, but connection is still alive.
                    continue;
                }
                perror("recv failed\n");
            } 
            if (bytes_received <= 0) {
                epoll_ctl(worker->epoll_fd, EPOLL_CTL_DEL, client_socket, NULL);
                close(client_socket);
                atomic_fetch_sub(&active_connections, 1);  // Use atomic fetch
                //if (debug_mode)
                    printf("[Worker %d] Closed connection %d\n", worker->thread_id, client_socket);
                continue;
            }

            buffer[bytes_received] = '\0';

            // Process request
            // Debug mode: Print received HTTP request
            if (debug_mode) {
                printf("\n--- Received Request ---\n%s\n-------------------------\n", buffer);
            }
            
            // Check if we received a full HTTP request (ends with double CRLF)
            if (strstr(buffer, "\r\n\r\n")) {
                atomic_fetch_add(&request_count, 1); // Increment request count
            
                const char *response =
                    "HTTP/1.1 200 OK\r\n"
                    "Content-Length: 13\r\n"
                    "Connection: keep-alive\r\n"
                    "Content-Type: text/plain\r\n\r\n"
                    "Hello, world!";
                if (delay)
                    usleep(delay * 1000);
            
                if (send_err_cycle) {
                    resp_cnt++;
                    if (resp_cnt >= send_err_cycle) {
                        resp_cnt = 0;
                        err_idx = (err_index_g++) % HTTP_ERROR_RESP_NUM;
                        if (err_index_g >= HTTP_ERROR_RESP_NUM)
                            err_index_g = 0;
                        response = err_response[err_idx++];
                    }
                }
                send(client_socket, response, strlen(response), 0);
            }
        }
    }
    return NULL;
}

// Accept new client and assign it to a worker
void accept_client() {
    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);
    int client_socket = accept(server_socket, (struct sockaddr *)&client_addr, &client_len);
    if (client_socket < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            // No more connections to accept right now
            usleep(2000);
            return;
        }
        perror("Accept failed");
        return;
    }

    set_nonblocking(client_socket);
    int worker_idx = next_worker;
    next_worker = (next_worker + 1) % MAX_WORKERS; // Round-robin selection

    // Add client socket to the selected worker's epoll instance
    struct epoll_event event;
    event.data.fd = client_socket;
    event.events = EPOLLIN | EPOLLET;
    epoll_ctl(workers[worker_idx].epoll_fd, EPOLL_CTL_ADD, client_socket, &event);
    atomic_fetch_add(&active_connections, 1);  // Use atomic fetch

    if (debug_mode)
        printf("[Main] Assigned connection %d to Worker %d\n", client_socket, worker_idx);
}

int main(int argc, char *argv[]) {
    int port = (argc > 1) ? atoi(argv[1]) : PORT;

    increase_fd_limit();

    // Parse command-line arguments
    if (argc > 1) {
        port = atoi(argv[1]);
        if (port <= 0 || port > 65535) {
            fprintf(stderr, "Invalid port number. Please specify a valid port (1-65535).\n");
            return EXIT_FAILURE;
        }
    }

    if (argc > 2) {
        for (int i = 2; i < argc; i++) {
            if (strstr(argv[i], "bg") || strstr(argv[i], "back")) {
                is_bg = 1;
                continue;
            }
            if (strstr(argv[i], "delay=")) {
                char *p = strchr(argv[i], '=');
                if (p) {
                    p++;
                    delay = atoi(p);
                    if (delay > 5000)
                        delay = 5000;
                    if (delay < 0)
                        delay = 0;
                    printf("Delay %d milliseconds.\n", delay);
                }
                continue;
            }
            if (strstr(argv[i], "errate=")) {
                char *p = strchr(argv[i], '=');
                if (p) {
                    p++;
                    errate = atof(p);
                    if (errate > 1)
                        errate = 1;
                    if (errate < 0)
                        errate = 0;
                    printf("Error Rate %f.\n", errate);
                    if (errate != 0) {
                        send_err_cycle = 1/errate;
                        if (send_err_cycle < 1)
                            send_err_cycle = 1;
                    }
                }
                continue;
            }
        }
    }

    server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket < 0) {
        perror("Socket creation failed");
        exit(EXIT_FAILURE);
    }

    int opt = 1;
    setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in server_addr = {0};
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(port);

    if (bind(server_socket, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("Bind failed");
        exit(EXIT_FAILURE);
    }

    if (listen(server_socket, SOMAXCONN) < 0) {
        perror("Listen failed");
        exit(EXIT_FAILURE);
    }

    set_nonblocking(server_socket);

    printf("Server listening on port %d...\n", port);

    if (!is_bg)
        printf("Press 'd' to toggle debug mode.\n");

    // Start stats thread
    pthread_t stats_tid;
    pthread_create(&stats_tid, NULL, stats_thread, NULL);
    pthread_detach(stats_tid);

    // Start debug input thread
    pthread_t debug_tid;
    pthread_create(&debug_tid, NULL, debug_toggle_thread, NULL);
    pthread_detach(debug_tid);

    // Create worker threads and epoll instances
    for (int i = 0; i < MAX_WORKERS; i++) {
        workers[i].epoll_fd = epoll_create1(0);
        workers[i].thread_id = i;
        pthread_create(&worker_threads[i], NULL, worker_function, &workers[i]);
    }

    while (1) {
        accept_client();
    }

    close(server_socket);
    return 0;
}
