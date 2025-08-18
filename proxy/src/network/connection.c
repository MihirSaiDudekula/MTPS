#include "../../include/network/connection.h"
#include "../../include/logger.h"
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include <netdb.h>
#include <sys/select.h>
#include <arpa/inet.h>

int server_init(int port) {
    int server_socket;
    struct sockaddr_in server_addr;
    int opt = 1;

    // Create socket
    if ((server_socket = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        LOG_ERROR("Socket creation failed");
        return -1;
    }

    // Set socket options to reuse address and port
    if (setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        LOG_ERROR("setsockopt(SO_REUSEADDR) failed");
        close(server_socket);
        return -1;
    }

    // Configure server address
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(port);

    // Bind socket to the port
    if (bind(server_socket, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        LOG_ERROR("Socket bind failed");
        close(server_socket);
        return -1;
    }

    // Start listening
    if (listen(server_socket, MAX_PENDING_CONNECTIONS) < 0) {
        LOG_ERROR("Listen failed");
        close(server_socket);
        return -1;
    }

    return server_socket;
}

int server_accept(int server_socket, client_info_t* client_info) {
    if (!client_info) return -1;
    
    client_info->addr_len = sizeof(client_info->client_addr);
    
    // Accept a new connection
    client_info->client_socket = accept(server_socket, 
                                      (struct sockaddr *)&client_info->client_addr,
                                      &client_info->addr_len);
    
    if (client_info->client_socket < 0) {
        LOG_ERROR("Accept failed");
        return -1;
    }
    
    // Set socket to non-blocking mode
    int flags = fcntl(client_info->client_socket, F_GETFL, 0);
    if (flags == -1 || fcntl(client_info->client_socket, F_SETFL, flags | O_NONBLOCK) == -1) {
        LOG_ERROR("Failed to set socket to non-blocking mode");
        close(client_info->client_socket);
        return -1;
    }
    
    return 0;
}

int connect_to_server(const char* host, int port, int timeout_ms) {
    int sockfd;
    struct sockaddr_in server_addr;
    struct hostent *server;
    struct timeval tv;
    
    // Create socket
    if ((sockfd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        LOG_ERROR("Socket creation failed");
        return -1;
    }
    
    // Set receive timeout
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    if (setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv)) < 0) {
        LOG_ERROR("setsockopt(SO_RCVTIMEO) failed");
        close(sockfd);
        return -1;
    }
    
    // Set send timeout
    if (setsockopt(sockfd, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tv, sizeof(tv)) < 0) {
        LOG_ERROR("setsockopt(SO_SNDTIMEO) failed");
        close(sockfd);
        return -1;
    }
    
    // Get server address
    server = gethostbyname(host);
    if (!server) {
        LOG_ERROR("No such host");
        close(sockfd);
        return -1;
    }
    
    // Configure server address
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    memcpy(&server_addr.sin_addr.s_addr, server->h_addr, server->h_length);
    server_addr.sin_port = htons(port);
    
    // Connect to server
    if (connect(sockfd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        if (errno != EINPROGRESS) {
            LOG_ERROR("Connection failed");
            close(sockfd);
            return -1;
        }
        
        // Handle non-blocking connect with timeout
        fd_set fdset;
        struct timeval tv_conn;
        
        FD_ZERO(&fdset);
        FD_SET(sockfd, &fdset);
        tv_conn.tv_sec = timeout_ms / 1000;
        tv_conn.tv_usec = (timeout_ms % 1000) * 1000;
        
        int res = select(sockfd + 1, NULL, &fdset, NULL, &tv_conn);
        if (res <= 0) {
            LOG_ERROR("Connection timeout");
            close(sockfd);
            return -1;
        }
        
        // Check for connection errors
        int so_error;
        socklen_t len = sizeof(so_error);
        getsockopt(sockfd, SOL_SOCKET, SO_ERROR, &so_error, &len);
        if (so_error) {
            LOG_ERROR("Connection failed");
            close(sockfd);
            return -1;
        }
    }
    
    return sockfd;
}

ssize_t forward_data(int from_fd, int to_fd, char* buffer, size_t buffer_size) {
    if (!buffer || buffer_size == 0) return -1;
    
    ssize_t bytes_read = recv(from_fd, buffer, buffer_size - 1, 0);
    if (bytes_read <= 0) {
        return bytes_read; // Error or connection closed
    }
    
    // Null-terminate the buffer for safety
    buffer[bytes_read] = '\0';
    
    // Send the data to the destination
    ssize_t bytes_sent = send(to_fd, buffer, bytes_read, 0);
    if (bytes_sent < 0) {
        LOG_ERROR("Failed to forward data");
        return -1;
    }
    
    return bytes_read;
}

void close_connection(int socket_fd) {
    if (socket_fd >= 0) {
        shutdown(socket_fd, SHUT_RDWR);
        close(socket_fd);
    }
}
