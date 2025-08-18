#include "../../include/http/request.h"
#include "../../include/logger.h"
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <errno.h>

// Case-insensitive string comparison
static int strcasecmp(const char* s1, const char* s2) {
    const unsigned char* p1 = (const unsigned char*)s1;
    const unsigned char* p2 = (const unsigned char*)s2;
    int result;
    
    if (p1 == p2) return 0;
    
    while ((result = tolower(*p1) - tolower(*p2)) == 0) {
        if (*p1++ == '\0') break;
        p2++;
    }
    
    return result;
}

// Case-insensitive string comparison with length
static int strncasecmp(const char* s1, const char* s2, size_t n) {
    if (n == 0) return 0;
    
    const unsigned char* p1 = (const unsigned char*)s1;
    const unsigned char* p2 = (const unsigned char*)s2;
    
    for (; n > 0; n--) {
        if (tolower(*p1) != tolower(*p2)) {
            return tolower(*p1) - tolower(*p2);
        }
        if (*p1 == '\0') break;
        p1++;
        p2++;
    }
    
    return 0;
}

int http_parse_request(const char* buffer, size_t length, HttpRequest* req) {
    if (!buffer || !req) return -1;
    
    // Initialize request structure
    memset(req, 0, sizeof(HttpRequest));
    
    // Make a copy of the buffer for parsing
    char* buf = strndup(buffer, length);
    if (!buf) {
        LOG_ERROR("Failed to allocate memory for request buffer");
        return -1;
    }
    
    // Parse request line (e.g., "GET /path HTTP/1.1")
    char* line = strtok(buf, "\r\n");
    if (!line) {
        free(buf);
        return -1;
    }
    
    // Parse method
    char* method_end = strchr(line, ' ');
    if (!method_end) {
        free(buf);
        return -1;
    }
    size_t method_len = method_end - line;
    if (method_len > MAX_METHOD_LENGTH) {
        free(buf);
        return -1;
    }
    strncpy(req->method, line, method_len);
    req->method[method_len] = '\0';
    
    // Parse URL
    char* url_start = method_end + 1;
    while (*url_start == ' ') url_start++;
    
    char* url_end = strchr(url_start, ' ');
    if (!url_end) {
        free(buf);
        return -1;
    }
    
    size_t url_len = url_end - url_start;
    if (url_len > MAX_URL_LENGTH) {
        free(buf);
        return -1;
    }
    strncpy(req->url, url_start, url_len);
    req->url[url_len] = '\0';
    
    // Parse HTTP version
    char* version_start = url_end + 1;
    while (*version_start == ' ') version_start++;
    
    if (strncmp(version_start, "HTTP/", 5) != 0) {
        free(buf);
        return -1;
    }
    
    size_t version_len = strcspn(version_start, " \t\r\n");
    if (version_len >= sizeof(req->version)) {
        free(buf);
        return -1;
    }
    strncpy(req->version, version_start, version_len);
    req->version[version_len] = '\0';
    
    // Parse headers
    char* header_line;
    size_t headers_len = 0;
    char* headers_start = line + strlen(line) + 2; // Skip request line and CRLF
    
    // Find the end of headers (empty line)
    char* body_start = strstr(headers_start, "\r\n\r\n");
    if (body_start) {
        headers_len = body_start - headers_start;
        body_start += 4; // Skip "\r\n\r\n"
    } else {
        headers_len = length - (headers_start - buf);
    }
    
    // Store headers
    if (headers_len > 0) {
        req->headers = strndup(headers_start, headers_len);
        if (!req->headers) {
            free(buf);
            return -1;
        }
        
        // Parse Content-Length and Content-Type
        char* header_ptr = req->headers;
        while ((header_line = strsep(&header_ptr, "\r\n")) && *header_line) {
            if (strncasecmp(header_line, "Host:", 5) == 0) {
                char* host_start = header_line + 5;
                while (*host_start == ' ') host_start++;
                strncpy(req->host, host_start, MAX_HOST_LENGTH);
                req->host[MAX_HOST_LENGTH] = '\0';
            }
            else if (strncasecmp(header_line, "Content-Length:", 15) == 0) {
                req->content_length = strtoul(header_line + 15, NULL, 10);
            }
            else if (strncasecmp(header_line, "Content-Type:", 13) == 0) {
                char* type_start = header_line + 13;
                while (*type_start == ' ') type_start++;
                strncpy(req->content_type, type_start, sizeof(req->content_type) - 1);
                req->content_type[sizeof(req->content_type) - 1] = '\0';
            }
        }
    }
    
    // Parse body if present
    if (body_start && req->content_length > 0) {
        size_t body_len = length - (body_start - buf);
        if (body_len > req->content_length) {
            body_len = req->content_length;
        }
        
        req->body = malloc(body_len + 1);
        if (req->body) {
            memcpy(req->body, body_start, body_len);
            req->body[body_len] = '\0';
        }
    }
    
    free(buf);
    return 0;
}

void http_request_cleanup(HttpRequest* req) {
    if (!req) return;
    
    if (req->headers) {
        free(req->headers);
        req->headers = NULL;
    }
    
    if (req->body) {
        free(req->body);
        req->body = NULL;
    }
    
    memset(req, 0, sizeof(HttpRequest));
}

const char* http_get_header(const HttpRequest* req, const char* header_name) {
    if (!req || !req->headers || !header_name) return NULL;
    
    size_t name_len = strlen(header_name);
    char* header_ptr = req->headers;
    char* header_line;
    
    while ((header_line = strsep(&header_ptr, "\r\n")) && *header_line) {
        if (strncasecmp(header_line, header_name, name_len) == 0 && 
            header_line[name_len] == ':') {
            char* value = header_line + name_len + 1;
            while (*value == ' ') value++; // Skip leading spaces
            return value;
        }
    }
    
    return NULL;
}
