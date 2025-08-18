#ifndef CACHE_H
#define CACHE_H

#include <stdbool.h>
#include <stddef.h>
#include <time.h>

// Cache element structure
typedef struct cache_element {
    char* data;               // Cached response data
    int len;                  // Length of data
    char* url;                // Request URL (key)
    time_t lru_time_track;    // Last access time
    struct cache_element* prev;
    struct cache_element* next;
} cache_element;

// Initialize the cache
void cache_init();

// Lookup an item in the cache
bool cache_lookup(const char* url, cache_element** element);

// Add an item to the cache
void cache_add(const char* url, const char* data, size_t len);

// Invalidate (remove) an item from the cache
void cache_invalidate(const char* url);

// Clean up the cache (free all memory)
void cache_cleanup();

// Get current cache statistics
void cache_get_stats(size_t* current_size, size_t* total_hits, size_t* total_misses);

#endif // CACHE_H
