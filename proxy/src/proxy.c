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

// Send all data, handling partial sends and retries
static ssize_t send_all(int sockfd, const void *buf, size_t len, int flags) {
    size_t total_sent = 0;
    const char *ptr = (const char *)buf;
    
    while (total_sent < len) {
        ssize_t sent = send(sockfd, ptr + total_sent, len - total_sent, flags);
        if (sent < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
                // Wait a bit before retrying
                struct timespec ts = {.tv_sec = 0, .tv_nsec = 10000000}; // 10ms
                nanosleep(&ts, NULL);
                continue;
            }
            return -1; // Error
        }
        if (sent == 0) {
            break; // Connection closed
        }
        total_sent += sent;
    }
    return total_sent;
}

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

    // Initialize the request struct
    memset(req, 0, sizeof(struct http_request));
    // now our goal is to extract the text from the request and fill the request struct

    // Check if the request is too large
    if (strlen(buffer) > MAX_BYTES - 1) {
        LOG_ERROR("Request too large");
        return;
    }

    // Parse request line: METHOD URL VERSION
    // a reques usually looks like this: GET / HTTP/1.1, so we need to find the first \r\n
    char* line_end = strstr(buffer, "\r\n");
    if (!line_end) {
        LOG_ERROR("Invalid request format - no CRLF");
        return;
    }
    
    *line_end = '\0'; // Temporarily null-terminate the first line
    
    // Parse method
    // the request usually looks like this: GET / HTTP/1.1, so we need to find the first space
    // the first space gives us the method
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
    // the request usually looks like this: GET / HTTP/1.1, so we need to find the second space
    // the second space gives us the URL
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
    // the request usually looks like this: GET / HTTP/1.1, so we need to find the third space
    // the third space gives us the version
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

// establishes a TCP connection from the proxy server to a backend/origin server
// host: The hostname or IP address of the backend server to connect to
// port: The port number of the backend server to connect to
// timeout_ms: The timeout in milliseconds for the connection attempt
// returns the socket descriptor of the connection, or -1 on failure

int connect_to_server(const char* host, int port, int timeout_ms) {
    // establishes a TCP connection to a server using a hostname and port, with a specified timeout
    struct hostent *server;
    // this is a special variable of hostent type which will store the details of our host
    struct sockaddr_in server_addr;
    // this is a special variable of sockaddr_in type which will store the details of our server
    struct timeval timeout;
    // this is a special variable of timeval type which will store the timeout value
    int server_socket;
    // this is a special variable of int type which will store the socket descriptor
    
    timeout.tv_sec = timeout_ms / 1000;
    timeout.tv_usec = (timeout_ms % 1000) * 1000;

    // gethostbyname is a function that returns a pointer to a hostent structure containing the host information, this is DNS lookup
    server = gethostbyname(host);
    if (!server) {
        LOG_ERROR("Could not resolve hostname");
        return -1;
    }

    // Set a timeout for receiving data on the socket.
    // If no data is received within the specified time (in 'timeout'), 
    // the recv() call will fail with a timeout error instead of blocking indefinitely.

    // Set socket options
    // SOL_SOCKET stands for socket level option
    // SO_RCVTIMEO used to set socket receive timeout
    // (const char*)&timeout is the timeout value pointer
    // socket is a function that creates a socket and returns a socket descriptor
    // here were creating a socket IPv4 and TCP protocol
    server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket < 0) {
        LOG_ERROR("Failed to create socket");
        return -1;
    }

    // setsockopt is a function that sets the options for a socket, the options being set here are receive and send timeout
    if (setsockopt(server_socket, SOL_SOCKET, SO_RCVTIMEO, 
                  (const char*)&timeout, sizeof(timeout)) < 0) {
        LOG_ERROR("Failed to set receive timeout");
        close(server_socket);
        return -1;
    }

    // setsockopt is a function that sets the options for a socket, the options being set here are receive and send timeout
    if (setsockopt(server_socket, SOL_SOCKET, SO_SNDTIMEO, 
                  (const char*)&timeout, sizeof(timeout)) < 0) {
        LOG_ERROR("Failed to set send timeout");
        close(server_socket);
        return -1;          
    }

    // Configure server address
    // Fill the server_addr structure with zeroes and clear out any garbage values
    // AF_INET is the address family for IPv4
    // htons is used to convert the port number to network byte order
    memset(&server_addr, 0, sizeof(server_addr));
    // specify IPv4 and port number, copy the IP address from the server into server_addr struct
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    memcpy(&server_addr.sin_addr.s_addr, server->h_addr_list[0], server->h_length);

    // connect is a function that connects to a server
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

// Invalidate cache for a specific URL
// we will be doing this when a post/put/delete request is received for that URL, because the data associated with that URL is modified
void cache_invalidate(const char* url) {
    pthread_mutex_lock(&cache_lock);

    // we will traverse the cache linked list to find the element
    cache_element* current = cache_head;
    while (current) {
        if (strcmp(current->url, url) == 0) {
            // remove the element from the cache , this is the equivalent of invalidating the cache
            remove_from_cache(current);
            safe_log("Invalidated cache for URL: %s", url);
            break;
        }
        current = current->next;
    }
    
    pthread_mutex_unlock(&cache_lock);
}

bool cache_lookup(const char* url, cache_element** element) {

    // we will use the cache_lock to ensure that the cache is not modified while we are looking up the cache
    pthread_mutex_lock(&cache_lock);
    

    cache_element* current = cache_head;
    
    // we will traverse the cache linked list to find the element
    while (current) {
        if (strcmp(current->url, url) == 0) {
            // Move the found item to front (MRU position)
            move_to_front(current);
            current->lru_time_track = time(NULL);

            // we will return the element to the caller, this is like pass by reference
            // although im returning true, im also returning the element pointer in the form of element reference
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
                    // if we are unable to allocate memory, we will remove the element from the cache
                    remove_from_cache(current);
                    pthread_mutex_unlock(&cache_lock);
                    return;
                }
                cache_stats.current_size += (len - current->len);
                total_cache_size += (len - current->len);
                current->len = len;
            }
            
            // update the data and the LRU time
            memcpy(current->data, data, len);
            current->lru_time_track = time(NULL);
            move_to_front(current);
            
            pthread_mutex_unlock(&cache_lock);
            return;
        }

        // move to the next element
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
    // arg is a void* which is the socket file descriptor coming from accepting incoming client request
    int client_socket = *(int*)arg;
    // free the arg, because we saved the value of the socket file descriptor in client_socket and now we don't need arg anymore
    free(arg);

    // wait for a connection semaphore to be available, that is until the semaphore count is greater than 0
    // semaphore count means the number of available connections
    sem_wait(&connection_semaphore);

    // set the socket timeout to 30 seconds    
    struct timeval timeout;
    timeout.tv_sec = 30;
    timeout.tv_usec = 0;

    // set the socket timeout for the client socket, this is to prevent the client from waiting for a response for a long time
    // If no data is received in 30 seconds, the connection will be closed
    // SOL_SOCKET is the level at which the option is defined, it is the socket level
    // SOL_RCVTIMEO is the option name, it is the receive timeout, which means the time to wait for data to be received
    if (setsockopt(client_socket, SOL_SOCKET, SO_RCVTIMEO, 
                  (const char*)&timeout, sizeof(timeout)) < 0) {
        LOG_ERROR("Failed to set client socket timeout");
        close(client_socket);
        sem_post(&connection_semaphore);
        return NULL;
    }

    // Receive the complete request, handling partial receives
    char *buffer = NULL;
    size_t total_received = 0;
    size_t buffer_size = MAX_BYTES;
    ssize_t bytes_received;
    char *header_end = NULL;
    size_t content_length = 0;
    bool headers_complete = false;
    bool has_body = false;

    // Allocate initial buffer
    buffer = malloc(buffer_size);
    if (!buffer) {
        LOG_ERROR("Memory allocation failed");
        send_error_response(client_socket, 500, "Internal Server Error");
        close(client_socket);
        sem_post(&connection_semaphore);
        return NULL;
    }

    // Read until we have all headers
    while (!headers_complete) {
        // Ensure we have space for at least one more byte and null terminator
        if (total_received >= buffer_size - 1) {
            buffer_size *= 2;
            char *new_buf = realloc(buffer, buffer_size);
            if (!new_buf) {
                LOG_ERROR("Failed to reallocate buffer");
                free(buffer);
                send_error_response(client_socket, 500, "Internal Server Error");
                close(client_socket);
                sem_post(&connection_semaphore);
                return NULL;
            }
            buffer = new_buf;
        }

        // Receive data
        bytes_received = recv(client_socket, buffer + total_received, 
                             buffer_size - total_received - 1, 0);
        
        if (bytes_received <= 0) {
            if (bytes_received == 0) {
                LOG_ERROR("Connection closed by client");
            } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
                LOG_ERROR("Failed to receive data from client");
            }
            free(buffer);
            close(client_socket);
            sem_post(&connection_semaphore);
            return NULL;
        }

        total_received += bytes_received;
        buffer[total_received] = '\0';

        // Check if we've received all headers
        header_end = strstr(buffer, "\r\n\r\n");
        if (header_end) {
            headers_complete = true;
            *header_end = '\0';  // Null-terminate headers for parsing
            
            // Check for Content-Length header if this is a POST/PUT request
            char *content_length_ptr = strcasestr(buffer, "Content-Length:");
            if (content_length_ptr) {
                content_length = strtoul(content_length_ptr + 15, NULL, 10);
                has_body = true;
                
                // Calculate how much of the body we've already received
                size_t headers_len = (header_end + 4 - buffer);
                size_t body_received = total_received - headers_len;
                
                // If we haven't received the full body yet, keep reading
                if (body_received < content_length) {
                    size_t remaining = content_length - body_received;
                    // Ensure we have enough space for remaining body
                    if (total_received + remaining >= buffer_size) {
                        buffer_size = total_received + remaining + 1;
                        char *new_buf = realloc(buffer, buffer_size);
                        if (!new_buf) {
                            LOG_ERROR("Failed to reallocate buffer for body");
                            free(buffer);
                            send_error_response(client_socket, 500, "Internal Server Error");
                            close(client_socket);
                            sem_post(&connection_semaphore);
                            return NULL;
                        }
                        buffer = new_buf;
                    }
                    
                    // Read remaining body
                    while (remaining > 0) {
                        bytes_received = recv(client_socket, buffer + total_received, 
                                           remaining, 0);
                        if (bytes_received <= 0) {
                            LOG_ERROR("Failed to receive complete request body");
                            free(buffer);
                            close(client_socket);
                            sem_post(&connection_semaphore);
                            return NULL;
                        }
                        total_received += bytes_received;
                        remaining -= bytes_received;
                    }
                    buffer[total_received] = '\0';
                }
            }
        }
    }

    safe_log("Received complete request (%zu bytes)", total_received);

    // Parse the request
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

    // cache_element_ptr is a pointer to the cache element, which is initially NULL
    cache_element* cache_element_ptr = NULL;
    char *cached_data = NULL;
    size_t cached_len = 0;

    // if the request is a GET request and the URL is in the cache, send the cached response
    if (strcmp(req.method, "GET") == 0 && cache_lookup(req.url, &cache_element_ptr)) {
        safe_log("Cache hit for URL: %s", req.url);
        
        // Make a copy of the cached data while holding the lock
        if (cache_element_ptr && cache_element_ptr->data && cache_element_ptr->len > 0) {
            cached_data = malloc(cache_element_ptr->len);
            if (cached_data) {
                memcpy(cached_data, cache_element_ptr->data, cache_element_ptr->len);
                cached_len = cache_element_ptr->len;
            }
        }
        
        // Release the cache lock by calling cache_lookup with NULL to indicate we're done
        cache_element_ptr = NULL;
        cache_lookup(NULL, &cache_element_ptr);
        
        // If we have valid cached data, send it
        if (cached_data && cached_len > 0) {
            if (send_all(client_socket, cached_data, cached_len, 0) < 0) {
                LOG_ERROR("Failed to send cached response to client");
            }
            free(cached_data);
            
            // close the client socket
            close(client_socket);
            
            // release the connection semaphore
            sem_post(&connection_semaphore);
            
            // return NULL to exit the thread
            return NULL;
        } else {
            // If we got here, we had a cache miss or error
            free(cached_data);
            cached_data = NULL;
        }
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

    // Forward request to target server using send_all to handle partial sends
    if (send_all(server_socket, buffer, total_received, 0) < 0) {
        LOG_ERROR("Failed to send complete request to target server");
        send_error_response(client_socket, 502, "Bad Gateway");
        close(server_socket);
        close(client_socket);
        free(buffer);
        sem_post(&connection_semaphore);
        return NULL;
    }
    free(buffer);  // Free request buffer as it's no longer needed

    // Receive and forward response
    char *response_buffer = NULL;
    size_t response_size = 0;
    size_t response_allocated = 0;
    bool should_cache = (strcmp(req.method, "GET") == 0);
    bool cacheable = false;
    bool chunked = false;
    size_t content_length = 0;
    bool headers_received = false;
    char *header_end = NULL;
    
    // Initial receive buffer
    char recv_buf[8192];
    
    while (true) {
        // Receive data from server
        ssize_t bytes = recv(server_socket, recv_buf, sizeof(recv_buf), 0);
        if (bytes <= 0) {
            if (bytes == 0) {
                safe_log("Server closed connection");
            } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
                LOG_ERROR("Error receiving from server");
            }
            break;
        }
        
        // Forward to client first (low latency)
        if (send_all(client_socket, recv_buf, bytes, 0) < 0) {
            LOG_ERROR("Failed to send complete response to client");
            break;
        }
        
        // If we're not caching, continue to next chunk
        if (!should_cache) continue;
        
        // Ensure we have enough space in the response buffer
        if (response_size + bytes > response_allocated) {
            size_t new_size = response_size + bytes + 8192;  // Extra space for next read
            char *new_buf = realloc(response_buffer, new_size);
            if (!new_buf) {
                LOG_ERROR("Failed to allocate memory for response");
                should_cache = false;
                continue;
            }
            response_buffer = new_buf;
            response_allocated = new_size;
        }
        
        // Append received data to response buffer
        memcpy(response_buffer + response_size, recv_buf, bytes);
        response_size += bytes;
        
        // Process headers if we haven't already
        if (!headers_received) {
            header_end = strstr(response_buffer, "\r\n\r\n");
            if (header_end) {
                headers_received = true;
                char *headers = response_buffer;
                *header_end = '\0';  // Null-terminate headers for parsing
                
                // Check if this is a cacheable response
                if (strstr(headers, "HTTP/1.1 200 ") || strstr(headers, "HTTP/1.0 200 ")) {
                    // Check for Transfer-Encoding: chunked
                    chunked = (strcasestr(headers, "Transfer-Encoding: chunked") != NULL);
                    
                    // Get Content-Length if not chunked
                    if (!chunked) {
                        char *cl_header = strcasestr(headers, "Content-Length:");
                        if (cl_header) {
                            content_length = strtoul(cl_header + 15, NULL, 10);
                            cacheable = true;
                            safe_log("Caching enabled, content length: %zu", content_length);
                        }
                    } else {
                        // Don't cache chunked responses for simplicity
                        should_cache = false;
                        safe_log("Not caching chunked response");
                    }
                } else {
                    // Only cache 200 OK responses
                    should_cache = false;
                }
                
                // Restore the original data
                *header_end = '\r';
            }
        }
        
        // If we know the content length, check if we've received everything
        if (cacheable && !chunked && response_size >= (header_end - response_buffer) + 4 + content_length) {
            safe_log("Received complete response, size: %zu", response_size);
            break;  // Received complete response
        }
    }
    
    // Cache the response if it's complete and cacheable
    if (should_cache && cacheable && response_buffer) {
        // Verify we have the complete response
        size_t headers_len = (header_end - response_buffer) + 4;  // +4 for \r\n\r\n
        // Only cache if we have the complete response
        if (response_size >= headers_len + content_length) {
            // Only cache if the response is within size limits
            if (response_size <= config.max_element_size) {
                // Make a copy of the response for the cache
                char *response_copy = malloc(response_size);
                if (response_copy) {
                    memcpy(response_copy, response_buffer, response_size);
                    cache_add(req.url, response_copy, response_size);
                    safe_log("Cached response for URL: %s (%zu bytes)", req.url, response_size);
                    // Don't free response_copy - it's now owned by the cache
                } else {
                    LOG_ERROR("Failed to allocate memory for cache copy");
                }
            } else {
                safe_log("Response too large to cache: %zu bytes", response_size);
            }
        } else {
            safe_log("Incomplete response received, not caching");
        }
    }
    
    // Clean up
    if (response_buffer) {
        free(response_buffer);
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
    // Set up signal handlers for graceful shutdown   
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    if (argc != 2) {
        // we need to provide the port number along with the program name as ./proxy <port_number>
        printf("Usage: %s <port_number>\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    int port = atoi(argv[1]);
    // check if the port number is valid using
    // atoi is used for converting the string to integer
    if (port <= 0 || port > 65535) {
        printf("Invalid port number. Use 1-65535\n");
        exit(EXIT_FAILURE);
    }

    config.port = port;

    // initialize the semaphore and mutex
    sem_init(&connection_semaphore, 0, MAX_CLIENTS);
    pthread_mutex_init(&cache_lock, NULL);

    // create the server socket
    // A server must set up a socket in advance — before any client can connect. 
    // This is how the operating system knows the server is available to accept incoming connections.
    int server_socket = socket(AF_INET, SOCK_STREAM, 0);
    // this is for clients
    // AF_INET is the address family for IPv4
    // SOCK_STREAM is the socket type for TCP
    // 0 is the protocol, which is set to 0 for the default protocol

    if (server_socket < 0) {
        perror("Failed to create socket");
        exit(EXIT_FAILURE);
    }

    int reuse = 1;
    // Allows the port to be reused quickly after server restarts — prevents “Address already in use” errors.
    setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    // describes how the server socket should behave — specifically, what kind of connections it's willing to accept.
    struct sockaddr_in server_addr = {0};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    server_addr.sin_addr.s_addr = INADDR_ANY;

    // bind the server socket to the address 
    // binds server_socket to listen on:
    // All available network interfaces (INADDR_ANY)
    // The specified port number (from command line arguments)
    // This is the proxy's own address, not the client's or server's.
    if (bind(server_socket, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("Binding failed");
        exit(EXIT_FAILURE);
    }

    // listen for incoming connections
    // The listen() function puts the server socket into a state where it can accept connections from clients.
    if (listen(server_socket, MAX_CLIENTS) < 0) {
        perror("Listen failed");
        exit(EXIT_FAILURE);
    }

    printf("Proxy server listening on port %d...\n", port);
    printf("Forwarding requests to %s:%d\n", config.target_host, config.target_port);


    while (keep_running) {
        // sockaddr is a structure that contains the address of the client coming from the socket library
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);

        // accept the connection
        
        // client_socket is the socket descriptor for the client connection of the size of int, we malloc it because we need to pass it to the thread
        int* client_socket = malloc(sizeof(int));
        if (!client_socket) {
            continue;
        }
        
        // The accept() function is used to accept a connection from a client.
        *client_socket = accept(server_socket, (struct sockaddr*)&client_addr, &client_len);

        if (*client_socket < 0) {
            perror("Accept failed");
            free(client_socket);
            continue;
        }

        // get the client's IP address
        char client_ip[INET_ADDRSTRLEN];

        // convert the IP address to a string
        inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, sizeof(client_ip));
        printf("New connection from %s:%d\n", client_ip, ntohs(client_addr.sin_port));

        // create a thread to handle the client connection
        // The code is using a multi-threaded approach where each client connection gets its own thread
        // thread_id is the id of the thread
        pthread_t thread_id;
        
        // pthread_create is used to create a thread, it takes 4 arguments, the first is the thread id, the second is the attributes of the thread, the third is the function to be executed by the thread, the fourth is the argument to the function
        // the reason why we use a thread for a socket here is to handle multiple clients at the same time
        if (pthread_create(&thread_id, NULL, handle_client, (void*)client_socket) != 0) {
            perror("Thread creation failed");
            close(*client_socket);
            free(client_socket);
            continue;
        }
        
        // pthread_detach is used to detach the thread, so that it can run independently
        pthread_detach(thread_id);
    }

    // cleanup resources, close the socket, destroy the semaphore and mutex
    close(server_socket);
    cache_cleanup();
    sem_destroy(&connection_semaphore);
    pthread_mutex_destroy(&cache_lock);
    return 0;
}