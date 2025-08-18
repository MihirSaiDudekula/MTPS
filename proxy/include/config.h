#ifndef CONFIG_H
#define CONFIG_H

#include <stdbool.h>
#include <stddef.h>

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

// Global configuration
extern ProxyConfig config;

// Cache statistics
typedef struct CacheStats {
    size_t total_hits;
    size_t total_misses;
    size_t current_size;
    size_t max_size;
} CacheStats;

extern CacheStats cache_stats;

// Default configuration values
#define DEFAULT_PORT 8080
#define DEFAULT_TARGET_PORT 3000
#define DEFAULT_MAX_CACHE_SIZE (200 * (1<<20))  // 200MB
#define DEFAULT_MAX_ELEMENT_SIZE (10 * (1<<20))  // 10MB
#define DEFAULT_MAX_CLIENTS 10
#define DEFAULT_DEBUG_MODE true

// Initialize configuration with default values
void init_default_config();

#endif // CONFIG_H
