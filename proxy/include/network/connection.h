#ifndef CONNECTION_H
#define CONNECTION_H

#include <sys/socket.h>
#include <netinet/in.h>

// Maximum number of pending connections in the queue
#define MAX_PENDING_CONNECTIONS 128

// Connection information structure
typedef struct {
    int client_socket;              // Client socket file descriptor
    struct sockaddr_in client_addr;  // Client address information
    socklen_t addr_len;             // Length of client address
} client_info_t;

// Initialize the server socket
// Returns socket file descriptor on success, -1 on error
int server_init(int port);

// Accept a new client connection
// Returns 0 on success, -1 on error
int server_accept(int server_socket, client_info_t* client_info);

// Connect to a backend server
// Returns socket file descriptor on success, -1 on error
int connect_to_server(const char* host, int port, int timeout_ms);

// Forward data from one socket to another
// Returns number of bytes forwarded, -1 on error
ssize_t forward_data(int from_fd, int to_fd, char* buffer, size_t buffer_size);

// Close a socket connection
void close_connection(int socket_fd);

#endif // CONNECTION_H
