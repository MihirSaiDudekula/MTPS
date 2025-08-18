#include "../../include/http/response.h"
#include "../../include/logger.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <time.h>

// Default status texts for common status codes
static const char* status_texts[] = {
    [100] = "Continue",
    [101] = "Switching Protocols",
    [200] = "OK",
    [201] = "Created",
    [204] = "No Content",
    [206] = "Partial Content",
    [301] = "Moved Permanently",
    [302] = "Found",
    [304] = "Not Modified",
    [307] = "Temporary Redirect",
    [308] = "Permanent Redirect",
    [400] = "Bad Request",
    [401] = "Unauthorized",
    [403] = "Forbidden",
    [404] = "Not Found",
    [405] = "Method Not Allowed",
    [408] = "Request Timeout",
    [413] = "Payload Too Large",
    [414] = "URI Too Long",
    [415] = "Unsupported Media Type",
    [429] = "Too Many Requests",
    [500] = "Internal Server Error",
    [501] = "Not Implemented",
    [502] = "Bad Gateway",
    [503] = "Service Unavailable",
    [504] = "Gateway Timeout",
    [505] = "HTTP Version Not Supported"
};

HttpResponse* http_response_create() {
    HttpResponse* res = calloc(1, sizeof(HttpResponse));
    if (!res) {
        LOG_ERROR("Failed to allocate memory for HTTP response");
        return NULL;
    }
    
    // Set default values
    res->status_code = 200;
    res->status_text = "OK";
    res->headers = NULL;
    res->body = NULL;
    res->content_length = 0;
    strcpy(res->version, "HTTP/1.1");
    strcpy(res->content_type, "text/plain; charset=utf-8");
    
    return res;
}

void http_response_free(HttpResponse* res) {
    if (!res) return;
    
    if (res->headers) free(res->headers);
    if (res->body) free(res->body);
    free(res);
}

void http_response_set_status(HttpResponse* res, int status_code, const char* status_text) {
    if (!res) return;
    
    res->status_code = status_code;
    
    // Use default status text if not provided
    if (status_text) {
        res->status_text = status_text;
    } else if (status_code >= 0 && status_code < (int)(sizeof(status_texts) / sizeof(status_texts[0])) && 
               status_texts[status_code] != NULL) {
        res->status_text = status_texts[status_code];
    } else {
        res->status_text = "Unknown Status";
    }
}

void http_response_set_header(HttpResponse* res, const char* name, const char* value) {
    if (!res || !name || !value) return;
    
    // Special handling for Content-Type
    if (strcasecmp(name, "Content-Type") == 0) {
        strncpy(res->content_type, value, sizeof(res->content_type) - 1);
        res->content_type[sizeof(res->content_type) - 1] = '\0';
        return;
    }
    
    // Special handling for Content-Length
    if (strcasecmp(name, "Content-Length") == 0) {
        res->content_length = strtoul(value, NULL, 10);
        return;
    }
    
    // For other headers, we'll rebuild the headers string when needed
    // This is a simplified implementation; a real implementation would use a list or map
    size_t new_header_len = strlen(name) + 2 + strlen(value) + 2; // name: value\r\n
    if (res->headers) {
        size_t old_len = strlen(res->headers);
        char* new_headers = realloc(res->headers, old_len + new_header_len + 1);
        if (!new_headers) {
            LOG_ERROR("Failed to reallocate memory for headers");
            return;
        }
        res->headers = new_headers;
        sprintf(res->headers + old_len, "%s: %s\r\n", name, value);
    } else {
        res->headers = malloc(new_header_len + 1);
        if (res->headers) {
            sprintf(res->headers, "%s: %s\r\n", name, value);
        }
    }
}

void http_response_set_body(HttpResponse* res, const char* body, size_t length) {
    if (!res) return;
    
    // Free existing body if any
    if (res->body) {
        free(res->body);
        res->body = NULL;
    }
    
    if (body && length > 0) {
        res->body = malloc(length);
        if (res->body) {
            memcpy(res->body, body, length);
            res->content_length = length;
        }
    } else {
        res->content_length = 0;
    }
}

char* http_response_to_string(const HttpResponse* res, size_t* length) {
    if (!res) return NULL;
    
    // Calculate required buffer size
    size_t status_line_len = snprintf(NULL, 0, "%s %d %s\r\n", 
                                    res->version, res->status_code, res->status_text);
    
    size_t content_type_len = snprintf(NULL, 0, "Content-Type: %s\r\n", res->content_type);
    size_t content_length_len = snprintf(NULL, 0, "Content-Length: %zu\r\n", res->content_length);
    
    // Add current date
    time_t now = time(NULL);
    char date_str[64];
    struct tm tm_info;
    gmtime_r(&now, &tm_info);
    strftime(date_str, sizeof(date_str), "%a, %d %b %Y %H:%M:%S GMT", &tm_info);
    size_t date_len = snprintf(NULL, 0, "Date: %s\r\n", date_str);
    
    size_t headers_len = res->headers ? strlen(res->headers) : 0;
    size_t total_len = status_line_len + content_type_len + content_length_len + 
                      date_len + headers_len + 4; // +4 for "\r\n\r\n"
    
    // Allocate buffer
    char* buffer = malloc(total_len + res->content_length + 1);
    if (!buffer) {
        LOG_ERROR("Failed to allocate memory for response buffer");
        return NULL;
    }
    
    // Build response
    char* p = buffer;
    p += sprintf(p, "%s %d %s\r\n", res->version, res->status_code, res->status_text);
    p += sprintf(p, "Date: %s\r\n", date_str);
    p += sprintf(p, "Content-Type: %s\r\n", res->content_type);
    p += sprintf(p, "Content-Length: %zu\r\n", res->content_length);
    
    // Add custom headers if any
    if (res->headers) {
        p += sprintf(p, "%s", res->headers);
    }
    
    // End of headers
    p += sprintf(p, "\r\n");
    
    // Add body if any
    if (res->body && res->content_length > 0) {
        memcpy(p, res->body, res->content_length);
        p += res->content_length;
    }
    
    *length = p - buffer;
    return buffer;
}

int http_send_error(int socket, int status_code, const char* message) {
    if (socket < 0) return -1;
    
    HttpResponse* res = http_response_create();
    if (!res) return -1;
    
    http_response_set_status(res, status_code, NULL);
    http_response_set_header(res, "Connection", "close");
    
    // Create error page
    char body[1024];
    snprintf(body, sizeof(body),
             "<!DOCTYPE html>\n"
             "<html>\n"
             "<head><title>%d %s</title></head>\n"
             "<body>\n"
             "<h1>%d %s</h1>\n"
             "<p>%s</p>\n"
             "<hr>\n"
             "<address>MTPS Proxy Server</address>\n"
             "</body>\n"
             "</html>",
             status_code, res->status_text,
             status_code, res->status_text,
             message ? message : "");
    
    http_response_set_body(res, body, strlen(body));
    
    // Convert to string and send
    size_t response_len;
    char* response = http_response_to_string(res, &response_len);
    
    int result = 0;
    if (response) {
        ssize_t sent = send(socket, response, response_len, 0);
        if (sent < 0) {
            LOG_ERROR("Failed to send error response");
            result = -1;
        }
        free(response);
    } else {
        result = -1;
    }
    
    http_response_free(res);
    return result;
}
