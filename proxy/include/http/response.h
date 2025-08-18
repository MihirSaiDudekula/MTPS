#ifndef HTTP_RESPONSE_H
#define HTTP_RESPONSE_H

#include <stddef.h>

// HTTP Response structure
typedef struct {
    int status_code;          // HTTP status code (200, 404, etc.)
    char* status_text;        // Status text (OK, Not Found, etc.)
    char* headers;            // Raw headers
    char* body;               // Response body
    size_t content_length;    // Content-Length header value
    char content_type[128];   // Content-Type header
    char version[16];         // HTTP version (e.g., HTTP/1.1)
} HttpResponse;

// Create a new HTTP response
HttpResponse* http_response_create();

// Free resources used by an HttpResponse
void http_response_free(HttpResponse* res);

// Set the status code and text
void http_response_set_status(HttpResponse* res, int status_code, const char* status_text);

// Set a header
void http_response_set_header(HttpResponse* res, const char* name, const char* value);

// Set the response body
void http_response_set_body(HttpResponse* res, const char* body, size_t length);

// Format the response as a string
char* http_response_to_string(const HttpResponse* res, size_t* length);

// Send an error response
int http_send_error(int socket, int status_code, const char* message);

#endif // HTTP_RESPONSE_H
