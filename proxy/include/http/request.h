#ifndef HTTP_REQUEST_H
#define HTTP_REQUEST_H

#include <stddef.h>

// Maximum sizes for request parsing
#define MAX_METHOD_LENGTH 15
#define MAX_URL_LENGTH 2047
#define MAX_HOST_LENGTH 255
#define MAX_HEADER_LENGTH 4096

// HTTP Request structure
typedef struct {
    char method[MAX_METHOD_LENGTH + 1];  // GET, POST, etc.
    char url[MAX_URL_LENGTH + 1];        // Request URL
    char host[MAX_HOST_LENGTH + 1];      // Host header
    char* headers;                       // Raw headers
    char* body;                          // Request body
    size_t content_length;               // Content-Length header value
    char content_type[128];              // Content-Type header
    char version[16];                    // HTTP version (e.g., HTTP/1.1)
} HttpRequest;

// Parse an HTTP request from a buffer
// Returns 0 on success, -1 on error
int http_parse_request(const char* buffer, size_t length, HttpRequest* req);

// Free resources used by an HttpRequest
void http_request_cleanup(HttpRequest* req);

// Get the value of a specific header
const char* http_get_header(const HttpRequest* req, const char* header_name);

#endif // HTTP_REQUEST_H
