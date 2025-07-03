/*
 * Multithreaded Proxy Server Implementation
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
// Case-insensitive string search
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

// A macro in C is a way to define a shortcut or code pattern that gets replaced by the preprocessor before the actual compilation happens
// Error logging macro - provides consistent error reporting. when i call the LOG_ERROR() method, it willm
#define LOG_ERROR(msg) fprintf(stderr, "ERROR: %s: %s\n", msg, strerror(errno))

// Configuration structure for proxy server settings
/**
 * @brief Configuration structure for the proxy server
 * Contains all configurable parameters that control the server's behavior
 */
typedef struct {
    int port;                // Port to listen on
    char* target_host;       // Backend server hostname
    int target_port;         // Backend server port
    size_t max_cache_size;   // Maximum cache size in bytes
    size_t max_element_size; // Maximum size of a single cached item
    int max_clients;         // Maximum concurrent clients
    bool debug_mode;         // Enable debug logging
} ProxyConfig;

/**
 * @brief Global configuration for the proxy server
 * Default values can be overridden through configuration file or command line
 */
ProxyConfig config = {
    .port = 8080,           // Default proxy port
    .target_host = "localhost", // Default backend host
    .target_port = 3000,    // Default backend port
    .max_cache_size = 200 * (1<<20), // 200MB cache
    .max_element_size = 10 * (1<<10), // 10KB max element
    .max_clients = 10,      // Max concurrent clients
    .debug_mode = false     // Debug logging disabled by default
};

/**
 * @brief Statistics tracking for cache performance
 * Maintains counters for cache hits, misses, and current size
 */
// Cache statistics structure
typedef struct CacheStats {
    size_t total_hits;       // Number of cache hits
    size_t total_misses;     // Number of cache misses
    size_t current_size;     // Current cache size in bytes
    size_t max_size;         // Maximum allowed cache size
} CacheStats;

// Global cache statistics
CacheStats cache_stats = {
    .total_hits = 0,
    .total_misses = 0,
    .current_size = 0,
    .max_size = 200 * (1<<20)  // 200MB default max size
};

/**
 * @brief Thread-safe logging function
 * Ensures that log messages are not interleaved when multiple threads are logging
 */
// In C, a variadic function can take a flexible number of arguments — which is useful when you don't know ahead of time how many values will be passed in
// va_list, va_start, va_end, and vprintf are part of <stdarg.h> — the C standard library for handling variable arguments.

// vprintf is like printf, but it takes a va_list instead of a variable number of arguments directl

void safe_log(const char* format, ...) {
    static pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER; // Static mutex for logging
    pthread_mutex_lock(&log_mutex); // Lock mutex to prevent concurrent writes
    
    va_list args;
    va_start(args, format);
    vprintf(format, args); // Print formatted message
    printf("\n");
    va_end(args);
    
    pthread_mutex_unlock(&log_mutex); // Release mutex
}

#define MAX_CLIENTS 10
#define MAX_BYTES 4096
#define MAX_ELEMENT_SIZE 10 * (1<<10)
#define MAX_CACHE_SIZE 200 * (1<<20)
#define DEFAULT_PORT 8080
#define MIN(a,b) ((a) < (b) ? (a) : (b))

// Define target server details
#define TARGET_HOST "localhost"
#define TARGET_PORT 3000

struct http_request {
    char method[16];         // e.g., "GET", "POST"
    char url[2048];          // Full URL requested
    char host[256];          // Host header from the request
    char* body;              // Pointer to the body of the request (e.g., POST data)
    int content_length;      // Length of the body in bytes
    char content_type[128];  // MIME type of the body, e.g., "application/json"
};


// linked list definition for the cache
typedef struct cache_element {
    char* data;                      // Pointer to the actual cached content (e.g., HTML, image)
    int len;                         // Length of the data in bytes
    char* url;                       // The URL associated with this cached content
    time_t lru_time_track;           // Time used for tracking "Least Recently Used" (LRU) policy
    struct cache_element* next;      // Pointer to the next cache element in the linked list
} cache_element;


pthread_t tid[MAX_CLIENTS];
// an array that Stores thread IDs for client connections

pthread_mutex_t cache_lock;
// A mutex (mutual exclusion lock) to protect shared access to the cache, Only one thread can hold the lock at a time — this prevents race conditions when multiple threads read/write the cache.

sem_t connection_semaphore;
// A semaphore (from `<semaphore.h>`) used to limit the number of concurrent client connections
// Before accepting a new client, a thread must `sem_wait(&connection_semaphore)`. When it finishes, it `sem_post(...)`.when it hits 0, no more clients can be handled until a thread releases a slot.


cache_element* cache_head = NULL;
// Pointer to the head of a linked list that stores cached content, initially null

int total_cache_size = 0;

bool validate_url(const char* url) {
    // Basic URL validation
    if (!url || strlen(url) > 2048) return false;
    // Makes sure the URL is not NULL and is not too long (over 2048 characters), which could cause buffer issues
    if (strchr(url, '\0') == NULL) return false;
    // Checks that the URL contains a null terminator

    // Check for potential proxy chaining - Proxy chaining = one proxy forwarding requests to another proxy.
    if (strchr(url, ':') != NULL) return false;
    if (strstr(url, "//") != NULL) return false;
    
    return true;
}

bool validate_headers(const char* headers) {
    // Check for malicious headers
    if (strstr(headers, "Proxy-Connection") != NULL) return false;
    if (strstr(headers, "X-Forwarded-For") != NULL) return false;
    if (strstr(headers, "X-Proxy") != NULL) return false;
    
    return true;
}

// Function prototypes
void parse_http_request(char* buffer, struct http_request* req);
int connect_to_server(const char* host, int port, int timeout_ms);
void cache_invalidate(const char* url);
bool cache_lookup(const char* url, struct cache_element** element);
void cache_add(const char* url, const char* data, size_t len);
void cache_cleanup();
void* handle_client(void* arg);
int send_error_response(int socket, int status_code);

// HTTP Request parsing
void parse_http_request(char* buffer, struct http_request* req) {
    // takes a raw HTTP request stored as a string (buffer) and extracts its components into a structured format (struct http_request* req). It performs basic parsing and validation step-by-step

    memset(req, 0, sizeof(struct http_request));
    // clears the req structure by zeroing out all bytes to start fresh.

    // Validate input length
    if (strlen(buffer) > MAX_BYTES - 1) {
        LOG_ERROR("Request too large");
        return;
    }

    // Parse method
    char* space = strchr(buffer, ' ');
    // Finds the first space in the buffer, which separates the method from the URL
    // space points to the first space character in that string

    // validation of the request method name, if we dont find the method name in the buffer
    // sizeof(req->method) is at max 16, so our req methods name cannot exceed 16
    if (!space || space - buffer > sizeof(req->method)) {
        LOG_ERROR("Invalid request method");
        return;
    }


    strncpy(req->method, buffer, space - buffer);
    // Copies characters from buffer into req->method, copying the space - buffer no. of characters

    req->method[space - buffer] = '\0';
    // terminate string

    // Parse URL
    char* url_start = space + 1;
    char* url_end = strchr(url_start, ' ');
    // look for the next space 

    if (!url_end || url_end - url_start > sizeof(req->url)) {
        LOG_ERROR("Invalid URL");
        return;
    }
    strncpy(req->url, url_start, url_end - url_start);
    req->url[url_end - url_start] = '\0';
    
    if (!validate_url(req->url)) {
        LOG_ERROR("Invalid URL format");
        return;
    }

    // Parse headers for keep-alive and content-length
    bool keep_alive = false;
    (void)keep_alive; // Mark as intentionally unused for now
    int content_length = 0;
    (void)content_length; // Mark as intentionally unused for now
    
    char* header_line = buffer;
    char* crlf = "\r\n";
    while ((header_line = strstr(header_line, crlf)) != NULL) {
        *header_line = '\0';  // Null-terminate the header line
        header_line += 2;      // Move past "\r\n"
        
        if (strncasecmp(header_line, "Connection: ", 12) == 0) {
            if (strcasestr(header_line + 12, "keep-alive")) {
                keep_alive = true;
            }
        } else if (strncasecmp(header_line, "Content-Length: ", 16) == 0) {
            content_length = atoi(header_line + 16);
        } else if (header_line[0] == '\r' && header_line[1] == '\n') {
            // End of headers
            break;
        }
    }
    
    // Restore the original buffer
    if (header_line) {
        *header_line = '\r';
    }
    
    // Parse request line and headers
    char* header_start = strstr(buffer, "\r\n");
    if (!header_start) {
        LOG_ERROR("Invalid request format");
        return;
    }
    
    if (!validate_headers(header_start)) {
        LOG_ERROR("Invalid headers");
        return;
    }

    // Parse content type
    char* content_type_hdr = strstr(buffer, "Content-Type: ");
    if (content_type_hdr) {
        content_type_hdr += 14; // Skip past the "Content-Type: " part
        char* content_type_end = strstr(content_type_hdr, "\r\n");
        
        if (content_type_end) {
            size_t len = (size_t)MIN(content_type_end - content_type_hdr, (long)(sizeof(req->content_type) - 1));
            strncpy(req->content_type, content_type_hdr, len);
            req->content_type[len] = '\0';
        }
    }

    // Parse content length
    char* content_length_hdr = strstr(buffer, "Content-Length: ");
    if (content_length_hdr) {
        content_length_hdr += 16; // Skip "Content-Length: "
        char* endptr;
        unsigned long length = strtoul(content_length_hdr, &endptr, 10);
        if (endptr == content_length_hdr || length > (unsigned long)MAX_ELEMENT_SIZE) {
            LOG_ERROR("Invalid content length");
            return;
        }
        req->content_length = (size_t)length;
    }

    // Parse body
    char* body = strstr(buffer, "\r\n\r\n");
    if (body) {
        req->body = body + 4;
    }
}

int connect_to_server(const char* host, int port, int timeout_ms) {
    // establishes a TCP connection to a server using a hostname and port, with a specified timeout

    struct hostent *server;
    // this is a special variable of hostent type which will store the details of our host

    struct sockaddr_in server_addr;
    // holds the server’s address information, such as IP address and port number, and is used when setting up the connection.

    struct timeval timeout;
    // specifies a timeout duration, typically used to set  timeouts for socket operations


    int server_socket;
    // in linux , everything is treated as a file, even a socket
    // the file descriptor for the socket used to connect to the server
    
    // timeout is important so that we dont wait perpetually for any inputs/outputs and communicate appropriate signals when theres any issue


    // Set socket timeout
    timeout.tv_sec = timeout_ms / 1000;
    timeout.tv_usec = (timeout_ms % 1000) * 1000;

    // obtain and resolve hostname
    server = gethostbyname(host);
    if (!server) {
        LOG_ERROR("Could not resolve hostname");
        return -1;
    }

    // Create socket
    // AF_INET means IPv4
    // SOCK_STREAM means TCP
    server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket < 0) {
        LOG_ERROR("Failed to create socket");
        return -1;
    }

    // Set a timeout for receiving data on the socket.
    // If no data is received within the specified time (in 'timeout'), 
    // the recv() call will fail with a timeout error instead of blocking indefinitely.

    // Set socket options
    // SOL_SOCKET stands for socket level option
    // SO_RCVTIMEO used to set socket receive timeout
    // (const char*)&timeout is the timeout value pointer
    if (setsockopt(server_socket, SOL_SOCKET, SO_RCVTIMEO, 
                  (const char*)&timeout, sizeof(timeout)) < 0) {
        LOG_ERROR("Failed to set receive timeout");
        close(server_socket);
        return -1;
    }

    // simliarly set send timeout
    if (setsockopt(server_socket, SOL_SOCKET, SO_SNDTIMEO, 
                  (const char*)&timeout, sizeof(timeout)) < 0) {
        LOG_ERROR("Failed to set send timeout");
        close(server_socket);
        return -1;
    }

    // Configure server address

    // Fill the server_addr structure with zeroes and clear out any garbage values
    memset(&server_addr, 0, sizeof(server_addr));

    // specify IPv4 and port number, copy the IP address from the server into server_addr struct
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    memcpy(&server_addr.sin_addr.s_addr, server->h_addr_list[0], server->h_length);

    // Connect with timeout
    // connect function needs the socket file descriptor, pointer to a sockaddr structure and length
    if (connect(server_socket, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        LOG_ERROR("Failed to connect to server");
        close(server_socket);
        return -1;
    }

    return server_socket;
}

// Invalidate cache for a specific URL
// we will be doing this when a post/put/delete request is received for that URL, because the data associated with that URL is modified
void cache_invalidate(const char* url) {
    pthread_mutex_lock(&cache_lock);
    
    cache_element* prev = NULL;
    cache_element* current = cache_head;
    
    while (current) {
        if (strcmp(current->url, url) == 0) {
            // Found the URL to invalidate
            if (prev == NULL) {
                // It's the head
                cache_head = current->next;
            } else {
                prev->next = current->next;
            }
            
            // Update cache stats
            cache_stats.current_size -= current->len;
            
            // Free memory
            free(current->url);
            free(current->data);
            cache_element* to_free = current;
            current = current->next;
            free(to_free);
            
            safe_log("Invalidated cache for URL: %s", url);
            continue;
        }
        prev = current;
        current = current->next;
    }
    
    pthread_mutex_unlock(&cache_lock);
}

// Cache management functions
bool cache_lookup(const char* url, cache_element** element) {
    // this function is just for locating the element in the cache for a cache hit -  true or false
    pthread_mutex_lock(&cache_lock);
    
    cache_element* prev = NULL;
    cache_element* current = cache_head;
    
    while (current) {
        if (strcmp(current->url, url) == 0) {
            // If found and not already at head
            if (prev != NULL) {
                // Remove from current position
                prev->next = current->next;
                // Move to head
                current->next = cache_head;
                cache_head = current;
            }
            
            *element = current;
            current->lru_time_track = time(NULL); // Update access time
            pthread_mutex_unlock(&cache_lock);
            
            // increment the cache hit no and return found true
            cache_stats.total_hits++;
            return true;
        }
        // this is simply pointer increment as part of linear search, similar to i++
        prev = current;
        current = current->next;
    }
    
    // and similarly, if not found
    pthread_mutex_unlock(&cache_lock);
    cache_stats.total_misses++;
    return false;
}

void cache_add(const char* url, const char* data, size_t len) {
    pthread_mutex_lock(&cache_lock);
    
    // Check if URL already exists
    cache_element* element;
    if (cache_lookup(url, &element)) {
        // Update existing element

        // cleanup all previous data
        free(element->data);
        // allocate memory for the element
        element->data = malloc(len);
        if (!element->data) {
            // not enough memory
            LOG_ERROR("Memory allocation failed for cache data");
            pthread_mutex_unlock(&cache_lock);
            return;
        }

        // update latest memory
        memcpy(element->data, data, len);
        element->len = len;

        // time NULL means fetch current time but dont store into any variable
        element->lru_time_track = time(NULL);
        pthread_mutex_unlock(&cache_lock);
        return;
    }

    // the item were looking for doesnt already exist so we need to Create new element
    cache_element* new_element = malloc(sizeof(cache_element));
    if (!new_element) {
        LOG_ERROR("Memory allocation failed for cache data");
        pthread_mutex_unlock(&cache_lock);
        return;
    }

    // copy necessary data into new element
    new_element->url = (char*)malloc(strlen(url) + 1);
    if (new_element->url) {
        strcpy(new_element->url, url);
    }
    new_element->data = malloc(len);
    if (!new_element->data) {
        // not enough memory
        free(new_element);
        pthread_mutex_unlock(&cache_lock);
        return;
    }
    memcpy(new_element->data, data, len);
    new_element->len = len;
    new_element->lru_time_track = time(NULL);

    // in this LRU cache implementation , we chose most recent element to be added at the head of the linkedlist, this make addition TC = O(1)
    new_element->next = cache_head;
    cache_head = new_element;

    // Update cache size - this is to help us know when to evict
    cache_stats.current_size += len;
    
    // Evict if cache is full - for eviction
    while (cache_stats.current_size > config.max_cache_size && cache_head) {
        cache_element* to_evict = cache_head;
        cache_element* prev_evict = NULL;
        cache_element* current = cache_head->next;
        cache_element* prev = cache_head;
        
        // Find the least recently used item
        while (current) {
            if (current->lru_time_track < to_evict->lru_time_track) {
                to_evict = current;
                prev_evict = prev;
            }
            prev = current;
            current = current->next;
        }

        if (to_evict) {
            // Remove from list
            if (to_evict == cache_head) {
                cache_head = to_evict->next;
            } else if (prev_evict) {
                prev_evict->next = to_evict->next;
            }
            
            // Update stats and free memory            
            cache_stats.current_size -= to_evict->len;
            free(to_evict->url);
            free(to_evict->data);
            free(to_evict);
        }
    }

    pthread_mutex_unlock(&cache_lock);
}

void cache_cleanup() {
    // this function is useful for cleaning up the cache when the proxy server is shutting down
    pthread_mutex_lock(&cache_lock);
    
    cache_element* current = cache_head;
    while (current) {
        // free all memory
        cache_element* next = current->next;
        free(current->url);
        free(current->data);
        free(current);
        current = next;
    }
    cache_head = NULL;
    cache_stats.current_size = 0;

    pthread_mutex_unlock(&cache_lock);
}

// to handle the client connection
void* handle_client(void* arg) {

    // arg is a void* which is the socket file descriptor coming from accepting incoming client request 
    int client_socket = *(int*)arg;
    free(arg);

    // Limit the number of concurrent client threads
    sem_wait(&connection_semaphore);

    // Set socket timeout
    struct timeval timeout;
    timeout.tv_sec = 30;  // 30 seconds
    timeout.tv_usec = 0;

    // If no data is received in 30 seconds, the connection will be closed
    if (setsockopt(client_socket, SOL_SOCKET, SO_RCVTIMEO, 
                  (const char*)&timeout, sizeof(timeout)) < 0) {
        // if setting timeout fails
        LOG_ERROR("Failed to set client socket timeout");
        close(client_socket);
        sem_post(&connection_semaphore);
        return NULL;
    }

    // data received from the client over the socket
    char buffer[MAX_BYTES];
    ssize_t bytes_received = recv(client_socket, buffer, sizeof(buffer) - 1, 0);

    if (bytes_received <= 0) {
        // if receiving data fails
        LOG_ERROR("Failed to receive data from client");
        close(client_socket);
        sem_post(&connection_semaphore);
        return NULL;
    }

    buffer[bytes_received] = '\0'; 
    // terminate the buffer once done

    struct http_request req;
    // parse the http request
    parse_http_request(buffer, &req);

    // check if the request is too large, closes connection if so
    if ((size_t)req.content_length > config.max_element_size) {
        // if request is too large
        LOG_ERROR("Request too large");
        send_error_response(client_socket, 413);
        close(client_socket);
        sem_post(&connection_semaphore);
        return NULL;
    }

    // Check cache first
    cache_element* cache_element;
    if (cache_lookup(req.url, &cache_element)) {
        // Cache hit - send cached response
        // the benefit of this is since the cache is hit the socket will send back the necessary data and theres no need to hit the main server
        safe_log("Cache hit for URL: %s", req.url);
        send(client_socket, cache_element->data, cache_element->len, 0);
        close(client_socket);
        sem_post(&connection_semaphore);
        return NULL;
    }

    // Check if we should invalidate cache for non-GET requests
    bool is_modifying_request = (strcmp(req.method, "POST") == 0 || 
                               strcmp(req.method, "PUT") == 0 || 
                               strcmp(req.method, "DELETE") == 0);
    
    if (is_modifying_request) {
        // Invalidate cache for this URL when it's modified
        cache_invalidate(req.url);
    }
    
    // Check for keep-alive
    bool keep_alive = false;
    const char* connection_hdr = strcasestr(buffer, "Connection: ");
    if (connection_hdr) {
        char* end = strstr(connection_hdr, "\r\n");
        if (end) {
            *end = '\0';
            if (strcasestr(connection_hdr, "keep-alive")) {
                keep_alive = true;
            }
            *end = '\r';
        }
    }
    
    // Connect to target server with keep-alive support
    int server_socket = connect_to_server(config.target_host, config.target_port, 5000);
    if (server_socket < 0) {
        // if connecting to target server fails
        LOG_ERROR("Failed to connect to target server");
        send_error_response(client_socket, 502);
        close(client_socket);
        sem_post(&connection_semaphore);
        return NULL;
    }

    // Forward request to target server
    if (send(server_socket, buffer, bytes_received, 0) < 0) {
        // if sending request to target server fails
        LOG_ERROR("Failed to send request to target server");
        send_error_response(client_socket, 500);
        close(server_socket);
        close(client_socket);
        sem_post(&connection_semaphore);
        return NULL;
    }

    // Buffer for response
    char response_buffer[MAX_BYTES];
    size_t total_received = 0;
    
    // Initialize response buffer for caching
    char* full_response = NULL;
    size_t allocated_size = 0;
    
    // Only cache GET requests
    bool should_cache = (strcmp(req.method, "GET") == 0);
    
    // Receive response in chunks from the target server
    while (true) {
        ssize_t bytes = recv(server_socket, response_buffer, sizeof(response_buffer), 0);
        if (bytes <= 0) break;
        
        // Send to client
        if (send(client_socket, response_buffer, bytes, 0) < 0) {
            LOG_ERROR("Failed to send to client");
            break;
        }
        
        // Only cache if it's a GET request and within size limits
        if (should_cache && (total_received + bytes) <= config.max_element_size) {
            // Resize buffer if needed
            if (total_received + bytes > allocated_size) {
                size_t new_size = total_received + bytes + MAX_BYTES;
                char* new_buf = realloc(full_response, new_size);
                if (!new_buf) {
                    LOG_ERROR("Memory allocation failed for response caching");
                    should_cache = false; // Stop trying to cache
                    free(full_response);
                    full_response = NULL;
                } else {
                    full_response = new_buf;
                    allocated_size = new_size;
                }
            }
            
            // Copy received data if we're still caching
            if (should_cache) {
                memcpy(full_response + total_received, response_buffer, bytes);
            }
        }
        
        total_received += bytes;
    }
    
    // Cache the complete response if we have it and it's a GET request
    if (should_cache && total_received > 0 && full_response) {
        // Don't cache error responses (4xx, 5xx)
        if (strncmp(full_response, "HTTP/1.", 7) == 0) {
            int status_code = atoi(full_response + 9); // Get status code from "HTTP/1.x XXX"
            if (status_code >= 200 && status_code < 300) {
                cache_add(req.url, full_response, total_received);
            }
        }
    }
    
    // Clean up
    if (full_response) {
        free(full_response);
    }

    // Handle keep-alive
    if (keep_alive) {
        // Look for Content-Length to determine if we need to read the body
        const char* content_length_hdr = strcasestr(response_buffer, "Content-Length: ");
        if (content_length_hdr) {
            long content_length = strtol(content_length_hdr + 16, NULL, 10);
            if (content_length > 0) {
                // We've already read some data, calculate remaining
                char* body_start = strstr(response_buffer, "\r\n\r\n");
                if (body_start) {
                    body_start += 4; // Skip past the header end
                    size_t header_size = body_start - response_buffer;
                    size_t body_read = total_received > header_size ? (total_received - header_size) : 0;
                    
                    // Read remaining body if needed
                    while (body_read < (size_t)content_length) {
                        ssize_t bytes = recv(server_socket, response_buffer, 
                                          MIN(sizeof(response_buffer), content_length - body_read), 0);
                        if (bytes <= 0) break;
                        
                        if (send(client_socket, response_buffer, bytes, 0) < 0) {
                            LOG_ERROR("Failed to send remaining body to client");
                            break;
                        }
                        body_read += bytes;
                    }
                }
            }
        }
        
        // Set keep-alive timeout
        struct timeval timeout;
        timeout.tv_sec = 30;  // 30 seconds
        timeout.tv_usec = 0;
        setsockopt(client_socket, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));
        
        // Don't close the sockets, just return to handle the next request
        close(server_socket);
        sem_post(&connection_semaphore);
        
        // Recursively handle the next request on the same connection
        handle_client(arg);
        return NULL;
    }
    
    // Close connection if not keep-alive
    close(server_socket);
    close(client_socket);
    sem_post(&connection_semaphore);
    return NULL;
}

int send_error_response(int socket, int status_code) {
    char response[512];
    char *status_text;

    switch(status_code) {
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

    // send error response to client when the request is invalid, this is visible on the html page
    snprintf(response, sizeof(response),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: application/json\r\n"
        "Connection: close\r\n"
        "\r\n"
        "{\"error\": \"%s\"}\r\n",
        status_code, status_text, status_text);
    
    return send(socket, response, strlen(response), 0);
}

// Signal handler for graceful shutdown
static volatile sig_atomic_t keep_running = 1;

void handle_signal(int sig) {
    (void)sig; // Unused parameter
    keep_running = 0;
}

int main(int argc, char* argv[]) {
    // Set up signal handlers for graceful shutdown
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    // validate the inputs 
    if (argc != 2) {
        // we need to provide the port number along with the program name as ./try <port_number>
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

    // initialize the semaphore and mutex
    sem_init(&connection_semaphore, 0, MAX_CLIENTS);
    pthread_mutex_init(&cache_lock, NULL);

    // A server must set up a socket in advance — before any client can connect. 
    // This is how the operating system knows the server is available to accept incoming connections.    
    int server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket < 0) {
        perror("Failed to create socket");
        exit(EXIT_FAILURE);
    }

    // Allows the port to be reused quickly after server restarts — prevents “Address already in use” errors.
    int reuse = 1;
    setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    // describes how the server socket should behave — specifically, what kind of connections it's willing to accept.
    struct sockaddr_in server_addr = {0};
    server_addr.sin_family = AF_INET; // IPv4
    server_addr.sin_port = htons(port); // port number
    server_addr.sin_addr.s_addr = INADDR_ANY; // accept connections from any IP address

    // bind the socket to the address
    if (bind(server_socket, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("Binding failed");
        exit(EXIT_FAILURE);
    }

    // listen for incoming connections
    if (listen(server_socket, MAX_CLIENTS) < 0) {
        perror("Listen failed");
        exit(EXIT_FAILURE);
    }

    printf("Proxy server listening on port %d...\n", port);

    while (keep_running) {
        // sockaddr is a structure that contains the address of the client coming from the socket library
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);

        int* client_socket = malloc(sizeof(int));
        *client_socket = accept(server_socket, (struct sockaddr*)&client_addr, &client_len);

        if (*client_socket < 0) {
            perror("Accept failed");
            free(client_socket);
            continue;
        }

        char client_ip[INET_ADDRSTRLEN];
        // inet_ntop is a function that converts an IP address from binary to a string
        inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, sizeof(client_ip));
        printf("New connection from %s:%d\n", client_ip, ntohs(client_addr.sin_port));

        pthread_t thread_id;
        // create a thread to handle the client
        if (pthread_create(&thread_id, NULL, handle_client, (void*)client_socket) != 0) {
            perror("Thread creation failed");
            close(*client_socket);
            free(client_socket);
            continue;
        }
        pthread_detach(thread_id);
    }

    // reach here once the server is terminated
    close(server_socket);
    sem_destroy(&connection_semaphore);
    pthread_mutex_destroy(&cache_lock);
    return 0;
}
