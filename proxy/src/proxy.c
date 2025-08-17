/*
 * Fixed Multithreaded Proxy Server Implementation
 * 
 * This proxy server acts as an intermediary between clients and a backend server,
 * providing caching capabilities to improve performance and reduce load on the
 * backend server.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <pthread.h>
#include <semaphore.h>
#include <time.h>
#include <errno.h>
#include <ctype.h>
#include <stdbool.h>
#include <sys/select.h>
#include <stdarg.h>
#include <sys/time.h>
#include <signal.h>
#include <sys/stat.h>
#include <fcntl.h>

// Define strcasestr if not available
#ifndef _GNU_SOURCE
char* strcasestr(const char* haystack, const char* needle) {
    if (!*needle) return (char*)haystack;
    
    for (; *haystack; ++haystack) {
        const char* h = haystack;
        const char* n = needle;
        
        while (tolower((unsigned char)*h) == tolower((unsigned char)*n) && *h && *n) {
            ++h;
            ++n;
        }
        
        if (!*n) {
            return (char*)haystack;
        }
    }
    
    return NULL;
}
#endif

#define LOG_ERROR(msg) fprintf(stderr, "ERROR: %s: %s\n", msg, strerror(errno))
#define LOG_DEBUG(msg) if (config.debug_mode) fprintf(stderr, "DEBUG: %s\n", msg)

// Configuration structure
typedef struct {
    int port;
    char* target_host;
    int target_port;
    size_t max_cache_size;
    size_t max_element_size;
    int max_clients;
    bool debug_mode;
} ProxyConfig;

ProxyConfig config = {
    .port = 8080,
    .target_host = "localhost",
    .target_port = 3000,
    .max_cache_size = 200 * (1<<20),
    .max_element_size = 10 * (1<<20),
    .max_clients = 10,
    .debug_mode = true
};

// Cache statistics
typedef struct CacheStats {
    size_t total_hits;
    size_t total_misses;
    size_t current_size;
    size_t max_size;
} CacheStats;

CacheStats cache_stats = {
    .total_hits = 0,
    .total_misses = 0,
    .current_size = 0,
    .max_size = 200 * (1<<20)
};

void safe_log(const char* format, ...) {
    static pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&log_mutex);
    
    va_list args;
    va_start(args, format);
    vprintf(format, args);
    printf("\n");
    fflush(stdout);
    va_end(args);
    
    pthread_mutex_unlock(&log_mutex);
}

#define MAX_CLIENTS 10
#define MAX_BYTES 65536
#define MAX_ELEMENT_SIZE 10 * (1<<20)
#define MAX_CACHE_SIZE 200 * (1<<20)

// Backend server configuration
#define NUM_BACKENDS 2
typedef struct {
    const char *host;
    int port;
} backend_server;

static backend_server backends[NUM_BACKENDS] = {
    {"server1", 3000},  // First backend server
    {"server2", 3000}   // Second backend server
};

static int current_backend = 0;
static pthread_mutex_t backend_lock = PTHREAD_MUTEX_INITIALIZER;
#define DEFAULT_PORT 8080
#define MIN(a,b) ((a) < (b) ? (a) : (b))
#define TARGET_HOST "localhost"
#define TARGET_PORT 3000

struct http_request {
    char method[16];
    char url[2048];
    char host[256];
    char* body;
    int content_length;
    char content_type[128];
    char version[16];
};

typedef struct cache_element {
    char* data;
    int len;
    char* url;
    time_t lru_time_track;
    struct cache_element* prev;
    struct cache_element* next;
} cache_element;

pthread_t tid[MAX_CLIENTS];
pthread_mutex_t cache_lock;
sem_t connection_semaphore;
cache_element* cache_head = NULL;
cache_element* cache_tail = NULL;
size_t total_cache_size = 0;
const size_t MAX_CACHE_SIZE = 200 * (1<<20); // 200MB

// Function prototypes
void parse_http_request(char* buffer, struct http_request* req);
int connect_to_server(const char* host, int port, int timeout_ms);
void cache_invalidate(const char* url);
bool cache_lookup(const char* url, cache_element** element);
void cache_add(const char* url, const char* data, size_t len);
void cache_cleanup();
void* handle_client(void* arg);
int send_error_response(int socket, int status_code, const char* message);

// Fixed HTTP Request parsing
void parse_http_request(char* buffer, struct http_request* req) {
    memset(req, 0, sizeof(struct http_request));
    
    if (strlen(buffer) > MAX_BYTES - 1) {
        LOG_ERROR("Request too large");
        return;
    }

    // Parse request line: METHOD URL VERSION
    char* line_end = strstr(buffer, "\r\n");
    if (!line_end) {
        LOG_ERROR("Invalid request format - no CRLF");
        return;
    }
    
    *line_end = '\0'; // Temporarily null-terminate the first line
    
    // Parse method
    char* space1 = strchr(buffer, ' ');
    if (!space1) {
        LOG_ERROR("Invalid request - no method");
        *line_end = '\r'; // Restore
        return;
    }
    
    size_t method_len = space1 - buffer;
    if (method_len >= sizeof(req->method)) {
        LOG_ERROR("Method too long");
        *line_end = '\r'; // Restore
        return;
    }
    
    strncpy(req->method, buffer, method_len);
    req->method[method_len] = '\0';
    
    // Parse URL
    char* url_start = space1 + 1;
    char* space2 = strchr(url_start, ' ');
    if (!space2) {
        LOG_ERROR("Invalid request - no URL");
        *line_end = '\r'; // Restore
        return;
    }
    
    size_t url_len = space2 - url_start;
    if (url_len >= sizeof(req->url)) {
        LOG_ERROR("URL too long");
        *line_end = '\r'; // Restore
        return;
    }
    
    strncpy(req->url, url_start, url_len);
    req->url[url_len] = '\0';
    
    // Parse version
    char* version_start = space2 + 1;
    size_t version_len = line_end - version_start;
    if (version_len >= sizeof(req->version)) {
        LOG_ERROR("Version too long");
        *line_end = '\r'; // Restore
        return;
    }
    
    strncpy(req->version, version_start, version_len);
    req->version[version_len] = '\0';
    
    *line_end = '\r'; // Restore the CRLF
    
    safe_log("Parsed request: %s %s %s", req->method, req->url, req->version);
}

// Get the next backend server in round-robin fashion
void get_next_backend(const char** host, int* port) {
    pthread_mutex_lock(&backend_lock);
    *host = backends[current_backend].host;
    *port = backends[current_backend].port;
    current_backend = (current_backend + 1) % NUM_BACKENDS;
    pthread_mutex_unlock(&backend_lock);
}

int connect_to_server(const char* host, int port, int timeout_ms) {
    struct hostent *server;
    struct sockaddr_in server_addr;
    struct timeval timeout;
    int server_socket;
    
    timeout.tv_sec = timeout_ms / 1000;
    timeout.tv_usec = (timeout_ms % 1000) * 1000;

    server = gethostbyname(host);
    if (!server) {
        LOG_ERROR("Could not resolve hostname");
        return -1;
    }

    server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket < 0) {
        LOG_ERROR("Failed to create socket");
        return -1;
    }

    if (setsockopt(server_socket, SOL_SOCKET, SO_RCVTIMEO, 
                  (const char*)&timeout, sizeof(timeout)) < 0) {
        LOG_ERROR("Failed to set receive timeout");
        close(server_socket);
        return -1;
    }

    if (setsockopt(server_socket, SOL_SOCKET, SO_SNDTIMEO, 
                  (const char*)&timeout, sizeof(timeout)) < 0) {
        LOG_ERROR("Failed to set send timeout");
        close(server_socket);
        return -1;
    }

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    memcpy(&server_addr.sin_addr.s_addr, server->h_addr_list[0], server->h_length);

    if (connect(server_socket, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        LOG_ERROR("Failed to connect to server");
        close(server_socket);
        return -1;
    }

    return server_socket;
}

// Remove a node from the cache
static void remove_from_cache(cache_element* node) {
    if (node->prev) {
        node->prev->next = node->next;
    } else {
        cache_head = node->next;
    }
    
    if (node->next) {
        node->next->prev = node->prev;
    } else {
        cache_tail = node->prev;
    }
    
    cache_stats.current_size -= node->len;
    total_cache_size -= node->len;
    free(node->url);
    free(node->data);
    free(node);
}

// Move a node to the front of the cache (MRU position)
static void move_to_front(cache_element* node) {
    if (node == cache_head) return; // Already at front
    
    // Remove from current position
    if (node->prev) node->prev->next = node->next;
    if (node->next) node->next->prev = node->prev;
    
    // Update tail if needed
    if (node == cache_tail) {
        cache_tail = node->prev;
    }
    
    // Add to front
    node->next = cache_head;
    node->prev = NULL;
    if (cache_head) {
        cache_head->prev = node;
    } else {
        cache_tail = node; // First element in cache
    }
    cache_head = node;
}

void cache_invalidate(const char* url) {
    pthread_mutex_lock(&cache_lock);
    
    cache_element* current = cache_head;
    while (current) {
        if (strcmp(current->url, url) == 0) {
            remove_from_cache(current);
            safe_log("Invalidated cache for URL: %s", url);
            break;
        }
        current = current->next;
    }
    
    pthread_mutex_unlock(&cache_lock);
}

bool cache_lookup(const char* url, cache_element** element) {
    pthread_mutex_lock(&cache_lock);
    
    cache_element* current = cache_head;
    
    while (current) {
        if (strcmp(current->url, url) == 0) {
            // Move the found item to front (MRU position)
            move_to_front(current);
            current->lru_time_track = time(NULL);
            
            *element = current;
            pthread_mutex_unlock(&cache_lock);
            
            cache_stats.total_hits++;
            return true;
        }
        current = current->next;
    }
    
    pthread_mutex_unlock(&cache_lock);
    cache_stats.total_misses++;
    return false;
}

void cache_add(const char* url, const char* data, size_t len) {
    if (len > config.max_element_size) {
        safe_log("Item too large for cache: %zu bytes (max: %zu)", len, config.max_element_size);
        return;
    }
    
    pthread_mutex_lock(&cache_lock);
    
    // Check if URL already exists in cache
    cache_element* current = cache_head;
    while (current) {
        if (strcmp(current->url, url) == 0) {
            // Update existing cache entry
            if (current->len != len) {
                // Size changed, need to reallocate
                free(current->data);
                current->data = malloc(len);
                if (!current->data) {
                    remove_from_cache(current);
                    pthread_mutex_unlock(&cache_lock);
                    return;
                }
                cache_stats.current_size += (len - current->len);
                total_cache_size += (len - current->len);
                current->len = len;
            }
            
            memcpy(current->data, data, len);
            current->lru_time_track = time(NULL);
            move_to_front(current);
            
            pthread_mutex_unlock(&cache_lock);
            return;
        }
        current = current->next;
    }
    
    // Evict LRU items if needed
    while (total_cache_size + len > MAX_CACHE_SIZE && cache_tail) {
        safe_log("Evicting LRU item: %s", cache_tail->url);
        remove_from_cache(cache_tail);
    }
    
    // Create new cache element
    cache_element* new_element = malloc(sizeof(cache_element));
    if (!new_element) {
        LOG_ERROR("Memory allocation failed for cache element");
        pthread_mutex_unlock(&cache_lock);
        return;
    }
    
    new_element->url = strdup(url);
    if (!new_element->url) {
        free(new_element);
        pthread_mutex_unlock(&cache_lock);
        return;
    }
    
    new_element->data = malloc(len);
    if (!new_element->data) {
        free(new_element->url);
        free(new_element);
        pthread_mutex_unlock(&cache_lock);
        return;
    }
    
    memcpy(new_element->data, data, len);
    new_element->len = len;
    new_element->lru_time_track = time(NULL);
    new_element->prev = NULL;
    new_element->next = cache_head;
    
    if (cache_head) {
        cache_head->prev = new_element;
    } else {
        cache_tail = new_element; // First element in cache
    }
    
    cache_head = new_element;
    cache_stats.current_size += len;
    total_cache_size += len;
    
    pthread_mutex_unlock(&cache_lock);
}

void cache_cleanup() {
    pthread_mutex_lock(&cache_lock);
    
    cache_element* current = cache_head;
    while (current) {
        cache_element* next = current->next;
        free(current->url);
        free(current->data);
        free(current);
        current = next;
    }
    
    cache_head = NULL;
    cache_tail = NULL;
    cache_stats.current_size = 0;
    total_cache_size = 0;
    
    pthread_mutex_unlock(&cache_lock);
}

void* handle_client(void* arg) {
    int client_socket = *(int*)arg;
    free(arg);

    sem_wait(&connection_semaphore);

    struct timeval timeout;
    timeout.tv_sec = 30;
    timeout.tv_usec = 0;

    if (setsockopt(client_socket, SOL_SOCKET, SO_RCVTIMEO, 
                  (const char*)&timeout, sizeof(timeout)) < 0) {
        LOG_ERROR("Failed to set client socket timeout");
        close(client_socket);
        sem_post(&connection_semaphore);
        return NULL;
    }

    char buffer[MAX_BYTES];
    ssize_t bytes_received = recv(client_socket, buffer, sizeof(buffer) - 1, 0);

    if (bytes_received <= 0) {
        LOG_ERROR("Failed to receive data from client");
        close(client_socket);
        sem_post(&connection_semaphore);
        return NULL;
    }

    buffer[bytes_received] = '\0';
    safe_log("Received request:\n%s", buffer);

    struct http_request req;
    parse_http_request(buffer, &req);

    // Validate parsed request
    if (strlen(req.method) == 0 || strlen(req.url) == 0) {
        LOG_ERROR("Invalid request - missing method or URL");
        send_error_response(client_socket, 400, "Bad Request");
        close(client_socket);
        sem_post(&connection_semaphore);
        return NULL;
    }

    // Check cache for GET requests
    cache_element* cache_element_ptr;
    if (strcmp(req.method, "GET") == 0 && cache_lookup(req.url, &cache_element_ptr)) {
        safe_log("Cache hit for URL: %s", req.url);
        send(client_socket, cache_element_ptr->data, cache_element_ptr->len, 0);
        close(client_socket);
        sem_post(&connection_semaphore);
        return NULL;
    }

    // Invalidate cache for modifying requests
    bool is_modifying_request = (strcmp(req.method, "POST") == 0 || 
                               strcmp(req.method, "PUT") == 0 || 
                               strcmp(req.method, "DELETE") == 0);
    
    if (is_modifying_request) {
        cache_invalidate(req.url);
    }
    
    // Connect to the next available backend server
    const char *backend_host;
    int backend_port;
    int server_socket = -1;
    int attempts = 0;
    
    // Try all backends in round-robin fashion until one succeeds or all fail
    while (attempts < NUM_BACKENDS) {
        get_next_backend(&backend_host, &backend_port);
        safe_log("Attempting to connect to backend: %s:%d", backend_host, backend_port);
        
        server_socket = connect_to_server(backend_host, backend_port, 5000);
        if (server_socket >= 0) {
            break;  // Connection successful
        }
        
        attempts++;
        safe_log("Connection to %s:%d failed, trying next server...", backend_host, backend_port);
    }
    
    if (server_socket < 0) {
        LOG_ERROR("All backend servers are unavailable");
        send_error_response(client_socket, 503, "Service Unavailable");
        close(client_socket);
        sem_post(&connection_semaphore);
        return NULL;
    }

    // Forward request to target server
    if (send(server_socket, buffer, bytes_received, 0) < 0) {
        LOG_ERROR("Failed to send request to target server");
        send_error_response(client_socket, 500, "Internal Server Error");
        close(server_socket);
        close(client_socket);
        sem_post(&connection_semaphore);
        return NULL;
    }

    // Receive and forward response
    char response_buffer[MAX_BYTES];
    char* full_response = NULL;
    size_t total_received = 0;
    size_t allocated_size = 0;
    bool should_cache = (strcmp(req.method, "GET") == 0);
    
    while (true) {
        ssize_t bytes = recv(server_socket, response_buffer, sizeof(response_buffer), 0);
        if (bytes <= 0) break;
        
        // Send to client
        if (send(client_socket, response_buffer, bytes, 0) < 0) {
            LOG_ERROR("Failed to send to client");
            break;
        }
        
        // Cache GET responses
        if (should_cache && (total_received + bytes) <= config.max_element_size) {
            if (total_received + bytes > allocated_size) {
                size_t new_size = total_received + bytes + MAX_BYTES;
                char* new_buf = realloc(full_response, new_size);
                if (!new_buf) {
                    LOG_ERROR("Memory allocation failed for response caching");
                    should_cache = false;
                    free(full_response);
                    full_response = NULL;
                } else {
                    full_response = new_buf;
                    allocated_size = new_size;
                }
            }
            
            if (should_cache) {
                memcpy(full_response + total_received, response_buffer, bytes);
            }
        }
        
        total_received += bytes;
    }
    
    // Cache successful GET responses
    if (should_cache && total_received > 0 && full_response) {
        if (strncmp(full_response, "HTTP/1.", 7) == 0) {
            int status_code = atoi(full_response + 9);
            if (status_code >= 200 && status_code < 300) {
                cache_add(req.url, full_response, total_received);
                safe_log("Cached response for URL: %s", req.url);
            }
        }
    }
    
    if (full_response) {
        free(full_response);
    }

    close(server_socket);
    close(client_socket);
    sem_post(&connection_semaphore);
    return NULL;
}

int send_error_response(int socket, int status_code, const char* message) {
    char response[1024];
    char *status_text;

    switch(status_code) {
        case 400:
            status_text = "Bad Request";
            break;
        case 405:
            status_text = "Method Not Allowed";
            break;
        case 502:
            status_text = "Bad Gateway";
            break;
        case 500:
            status_text = "Internal Server Error";
            break;
        default:
            status_text = "Internal Server Error";
            status_code = 500;
    }

    snprintf(response, sizeof(response),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: text/plain\r\n"
        "Connection: close\r\n"
        "Content-Length: %lu\r\n"
        "\r\n"
        "%s\r\n",
        status_code, status_text, strlen(message), message);
    
    return send(socket, response, strlen(response), 0);
}

// Signal handler for graceful shutdown
static volatile sig_atomic_t keep_running = 1;

void handle_signal(int sig) {
    (void)sig;
    keep_running = 0;
}

int main(int argc, char* argv[]) {
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    if (argc != 2) {
        printf("Usage: %s <port_number>\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    int port = atoi(argv[1]);
    if (port <= 0 || port > 65535) {
        printf("Invalid port number. Use 1-65535\n");
        exit(EXIT_FAILURE);
    }

    config.port = port;

    sem_init(&connection_semaphore, 0, MAX_CLIENTS);
    pthread_mutex_init(&cache_lock, NULL);

    int server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket < 0) {
        perror("Failed to create socket");
        exit(EXIT_FAILURE);
    }

    int reuse = 1;
    setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in server_addr = {0};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    server_addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(server_socket, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("Binding failed");
        exit(EXIT_FAILURE);
    }

    if (listen(server_socket, MAX_CLIENTS) < 0) {
        perror("Listen failed");
        exit(EXIT_FAILURE);
    }

    printf("Proxy server listening on port %d...\n", port);
    printf("Forwarding requests to %s:%d\n", config.target_host, config.target_port);

    while (keep_running) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);

        int* client_socket = malloc(sizeof(int));
        if (!client_socket) {
            continue;
        }
        
        *client_socket = accept(server_socket, (struct sockaddr*)&client_addr, &client_len);

        if (*client_socket < 0) {
            perror("Accept failed");
            free(client_socket);
            continue;
        }

        char client_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, sizeof(client_ip));
        printf("New connection from %s:%d\n", client_ip, ntohs(client_addr.sin_port));

        pthread_t thread_id;
        if (pthread_create(&thread_id, NULL, handle_client, (void*)client_socket) != 0) {
            perror("Thread creation failed");
            close(*client_socket);
            free(client_socket);
            continue;
        }
        pthread_detach(thread_id);
    }

    close(server_socket);
    cache_cleanup();
    sem_destroy(&connection_semaphore);
    pthread_mutex_destroy(&cache_lock);
    return 0;
}