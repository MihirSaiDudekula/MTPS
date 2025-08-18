#include "../include/config.h"
#include "../include/logger.h"
#include "../include/cache.h"
#include "../include/http/request.h"
#include "../include/http/response.h"
#include "../include/network/connection.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <pthread.h>
#include <unistd.h>

// Global variables
static volatile sig_atomic_t keep_running = 1;
ProxyConfig config;
CacheStats cache_stats;

// Function prototypes
void handle_signal(int sig);
void* handle_client(void* arg);
void init_default_config();

int main(int argc, char* argv[]) {
    // Set up signal handling
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);
    
    // Initialize default configuration
    init_default_config();
    
    // Initialize cache
    cache_init();
    
    // Initialize server socket
    int server_socket = server_init(config.port);
    if (server_socket < 0) {
        LOG_ERROR("Failed to initialize server");
        return 1;
    }
    
    safe_log("Proxy server started on port %d", config.port);
    safe_log("Forwarding to %s:%d", config.target_host, config.target_port);
    
    // Main server loop
    while (keep_running) {
        client_info_t client_info;
        
        // Accept a new client connection
        if (server_accept(server_socket, &client_info) == 0) {
            // Create a new thread to handle the client
            pthread_t thread_id;
            if (pthread_create(&thread_id, NULL, handle_client, &client_info) != 0) {
                LOG_ERROR("Failed to create thread");
                close_connection(client_info.client_socket);
                continue;
            }
            
            // Detach the thread (we won't be joining with it)
            pthread_detach(thread_id);
        }
    }
    
    // Cleanup
    safe_log("Shutting down server...");
    close_connection(server_socket);
    cache_cleanup();
    
    return 0;
}

void* handle_client(void* arg) {
    if (!arg) return NULL;
    
    client_info_t* client_info = (client_info_t*)arg;
    int client_socket = client_info->client_socket;
    char client_ip[INET_ADDRSTRLEN];
    
    // Get client IP address
    inet_ntop(AF_INET, &(client_info->client_addr.sin_addr), client_ip, INET_ADDRSTRLEN);
    safe_log("New connection from %s", client_ip);
    
    char buffer[8192];
    ssize_t bytes_read;
    
    // Read client request
    bytes_read = recv(client_socket, buffer, sizeof(buffer) - 1, 0);
    if (bytes_read <= 0) {
        close_connection(client_socket);
        return NULL;
    }
    
    // Null-terminate the request
    buffer[bytes_read] = '\0';
    
    // Parse HTTP request
    HttpRequest req;
    if (http_parse_request(buffer, bytes_read, &req) != 0) {
        http_send_error(client_socket, 400, "Bad Request");
        http_request_cleanup(&req);
        close_connection(client_socket);
        return NULL;
    }
    
    // Check if the request is in the cache
    cache_element* cached = NULL;
    if (strcmp(req.method, "GET") == 0 && cache_lookup(req.url, &cached)) {
        // Cache hit - send cached response
        safe_log("Cache hit for %s", req.url);
        send(client_socket, cached->data, cached->len, 0);
        http_request_cleanup(&req);
        close_connection(client_socket);
        return NULL;
    }
    
    // Cache miss - forward request to backend server
    safe_log("Cache miss for %s", req.url);
    
    int backend_socket = connect_to_server(config.target_host, config.target_port, 5000);
    if (backend_socket < 0) {
        http_send_error(client_socket, 502, "Bad Gateway");
        http_request_cleanup(&req);
        close_connection(client_socket);
        return NULL;
    }
    
    // Forward the original request to the backend
    send(backend_socket, buffer, bytes_read, 0);
    
    // Forward the response from backend to client and cache it if it's a GET request
    char response_buffer[8192];
    size_t total_response_size = 0;
    char* full_response = NULL;
    size_t full_response_size = 0;
    
    while ((bytes_read = recv(backend_socket, response_buffer, sizeof(response_buffer) - 1, 0)) > 0) {
        // Forward to client
        send(client_socket, response_buffer, bytes_read, 0);
        
        // If this is a GET request, cache the response
        if (strcmp(req.method, "GET") == 0) {
            // Reallocate buffer for full response
            char* new_buffer = realloc(full_response, full_response_size + bytes_read + 1);
            if (!new_buffer) {
                LOG_ERROR("Failed to allocate memory for response caching");
                break;
            }
            full_response = new_buffer;
            memcpy(full_response + full_response_size, response_buffer, bytes_read);
            full_response_size += bytes_read;
            full_response[full_response_size] = '\0';
            total_response_size += bytes_read;
            
            // Don't cache responses that are too large
            if (total_response_size > config.max_element_size) {
                free(full_response);
                full_response = NULL;
            }
        }
    }
    
    // Cache the response if we have one
    if (strcmp(req.method, "GET") == 0 && full_response && full_response_size > 0) {
        cache_add(req.url, full_response, full_response_size);
        free(full_response);
    }
    
    // Cleanup
    http_request_cleanup(&req);
    close_connection(backend_socket);
    close_connection(client_socket);
    
    return NULL;
}

void init_default_config() {
    config.port = DEFAULT_PORT;
    config.target_port = DEFAULT_TARGET_PORT;
    config.max_cache_size = DEFAULT_MAX_CACHE_SIZE;
    config.max_element_size = DEFAULT_MAX_ELEMENT_SIZE;
    config.max_clients = DEFAULT_MAX_CLIENTS;
    config.debug_mode = DEFAULT_DEBUG_MODE;
    
    // Allocate and copy the target host
    config.target_host = strdup("localhost");
    if (!config.target_host) {
        LOG_ERROR("Failed to allocate memory for target host");
        exit(1);
    }
    
    // Initialize cache stats
    cache_stats = (CacheStats){
        .total_hits = 0,
        .total_misses = 0,
        .current_size = 0,
        .max_size = config.max_cache_size
    };
}

void handle_signal(int sig) {
    if (sig == SIGINT || sig == SIGTERM) {
        keep_running = 0;
    }
}
