#include "../../include/cache.h"
#include "../../include/config.h"
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

// Global cache state
static cache_element* cache_head = NULL;
static cache_element* cache_tail = NULL;
static size_t total_cache_size = 0;
static pthread_mutex_t cache_lock = PTHREAD_MUTEX_INITIALIZER;

// Forward declarations
static void remove_from_cache(cache_element* node);
static void move_to_front(cache_element* node);

void cache_init() {
    // Initialize cache statistics
    cache_stats = (CacheStats){
        .total_hits = 0,
        .total_misses = 0,
        .current_size = 0,
        .max_size = config.max_cache_size
    };
}

// Remove a node from the cache
static void remove_from_cache(cache_element* node) {
    if (!node) return;
    
    if (node->prev) {
        node->prev->next = node->next;
    } else {
        cache_head = node->next;
    }
    
    if (node->next) {
        node->next->prev = node->prev;
    } else {
        cache_tail = node->prev;
    }
    
    cache_stats.current_size -= node->len;
    total_cache_size -= node->len;
    free(node->url);
    free(node->data);
    free(node);
}

// Move a node to the front of the cache (MRU position)
static void move_to_front(cache_element* node) {
    if (!node || node == cache_head) return;
    
    // Remove from current position
    if (node->prev) node->prev->next = node->next;
    if (node->next) node->next->prev = node->prev;
    
    // Update tail if needed
    if (node == cache_tail) {
        cache_tail = node->prev;
    }
    
    // Add to front
    node->next = cache_head;
    node->prev = NULL;
    if (cache_head) {
        cache_head->prev = node;
    } else {
        cache_tail = node; // First element in cache
    }
    cache_head = node;
}

bool cache_lookup(const char* url, cache_element** element) {
    pthread_mutex_lock(&cache_lock);
    
    cache_element* current = cache_head;
    bool found = false;
    
    while (current) {
        if (strcmp(current->url, url) == 0) {
            // Move the found item to front (MRU position)
            move_to_front(current);
            current->lru_time_track = time(NULL);
            
            *element = current;
            cache_stats.total_hits++;
            found = true;
            break;
        }
        current = current->next;
    }
    
    if (!found) {
        cache_stats.total_misses++;
    }
    
    pthread_mutex_unlock(&cache_lock);
    return found;
}

void cache_add(const char* url, const char* data, size_t len) {
    if (len > config.max_element_size) {
        // Item too large for cache
        return;
    }
    
    pthread_mutex_lock(&cache_lock);
    
    // Check if URL already exists in cache
    cache_element* current = cache_head;
    while (current) {
        if (strcmp(current->url, url) == 0) {
            // Update existing cache entry
            if (current->len != len) {
                // Size changed, need to reallocate
                free(current->data);
                current->data = malloc(len);
                if (!current->data) {
                    remove_from_cache(current);
                    pthread_mutex_unlock(&cache_lock);
                    return;
                }
                cache_stats.current_size += (len - current->len);
                total_cache_size += (len - current->len);
                current->len = len;
            }
            
            memcpy(current->data, data, len);
            current->lru_time_track = time(NULL);
            move_to_front(current);
            
            pthread_mutex_unlock(&cache_lock);
            return;
        }
        current = current->next;
    }
    
    // Evict LRU items if needed
    while (total_cache_size + len > config.max_cache_size && cache_tail) {
        remove_from_cache(cache_tail);
    }
    
    // Create new cache element
    cache_element* new_element = malloc(sizeof(cache_element));
    if (!new_element) {
        pthread_mutex_unlock(&cache_lock);
        return;
    }
    
    new_element->url = strdup(url);
    if (!new_element->url) {
        free(new_element);
        pthread_mutex_unlock(&cache_lock);
        return;
    }
    
    new_element->data = malloc(len);
    if (!new_element->data) {
        free(new_element->url);
        free(new_element);
        pthread_mutex_unlock(&cache_lock);
        return;
    }
    
    memcpy(new_element->data, data, len);
    new_element->len = len;
    new_element->lru_time_track = time(NULL);
    new_element->prev = NULL;
    new_element->next = cache_head;
    
    if (cache_head) {
        cache_head->prev = new_element;
    } else {
        cache_tail = new_element; // First element in cache
    }
    
    cache_head = new_element;
    cache_stats.current_size += len;
    total_cache_size += len;
    
    pthread_mutex_unlock(&cache_lock);
}

void cache_invalidate(const char* url) {
    pthread_mutex_lock(&cache_lock);
    
    cache_element* current = cache_head;
    while (current) {
        if (strcmp(current->url, url) == 0) {
            remove_from_cache(current);
            break;
        }
        current = current->next;
    }
    
    pthread_mutex_unlock(&cache_lock);
}

void cache_cleanup() {
    pthread_mutex_lock(&cache_lock);
    
    cache_element* current = cache_head;
    while (current) {
        cache_element* next = current->next;
        free(current->url);
        free(current->data);
        free(current);
        current = next;
    }
    
    cache_head = NULL;
    cache_tail = NULL;
    cache_stats.current_size = 0;
    total_cache_size = 0;
    
    pthread_mutex_unlock(&cache_lock);
}

void cache_get_stats(size_t* current_size, size_t* total_hits, size_t* total_misses) {
    pthread_mutex_lock(&cache_lock);
    if (current_size) *current_size = cache_stats.current_size;
    if (total_hits) *total_hits = cache_stats.total_hits;
    if (total_misses) *total_misses = cache_stats.total_misses;
    pthread_mutex_unlock(&cache_lock);
}
