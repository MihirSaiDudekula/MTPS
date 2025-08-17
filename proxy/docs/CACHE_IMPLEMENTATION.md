# Proxy Server Cache Implementation

## System Flow

### 1. System Initialization

When the proxy server starts:
- The `main()` function initializes the system
- Configuration is loaded (port, target host, cache sizes, etc.)
- The server socket is created and set to listen for incoming connections
- Worker threads are created to handle client connections

### 2. Client Request Handling Flow

#### A. New Client Connection
1. **Client Connects**: A client makes an HTTP request to the proxy
2. **Connection Accepted**: The main thread accepts the connection and passes it to `handle_client()`

#### B. Request Processing in `handle_client()`
1. **Read Request**: The client's HTTP request is read from the socket
2. **Parse Request**: `parse_http_request()` extracts:
   - HTTP method (GET, POST, etc.)
   - Request URL
   - Headers
   - Body (if any)

### 3. Cache Lookup Flow

#### A. Cache Miss (First Request)
1. **Check Cache**: `cache_lookup(url, &cached)` is called
2. **Cache Miss**:
   - If URL not found, returns `false`
   - `cache_stats.total_misses` is incremented
3. **Forward Request**:
   - The request is forwarded to the backend server using `connect_to_server()`
   - Response is received from the server
4. **Cache Population**:
   - `cache_add(url, response_data, response_size)` is called
   - If the response size is within limits (`max_element_size`), it's cached
   - The new item is added to the front of the list (MRU position)

#### B. Cache Hit (Subsequent Request)
1. **Check Cache**: `cache_lookup(url, &cached)` is called
2. **Cache Hit**:
   - If URL is found, the item is moved to the front of the list
   - `cache_stats.total_hits` is incremented
   - The cached response is sent directly to the client
   - No need to contact the backend server

### 4. Cache Management

#### A. Adding New Items (`cache_add()`)
1. **Size Validation**:
   - If item size > `max_element_size`, it's not cached
   - Logs "Item too large for cache" message
2. **Update Existing**:
   - If URL already exists, updates the existing entry
   - Handles reallocation if the new size is different
3. **LRU Eviction**:
   ```c
   while (total_cache_size + len > MAX_CACHE_SIZE && cache_tail) {
       safe_log("Evicting LRU item: %s", cache_tail->url);
       remove_from_cache(cache_tail);
   }
   ```
   - If adding would exceed `MAX_CACHE_SIZE`, evicts LRU items from the tail
4. **Add New Item**:
   - Creates new cache entry
   - Adds to the front of the list
   - Updates size counters

#### B. Cache Lookup (`cache_lookup()`)
1. **Linear Search**:
   - Searches through the list for the URL
   - If found, moves the item to the front using `move_to_front()`
   - Updates the last access time

#### C. Cache Invalidation (`cache_invalidate()`)
1. **Find and Remove**:
   - Searches for the URL in the cache
   - If found, removes it using `remove_from_cache()`
   - Updates size counters

### 5. Cache Data Structure

#### Doubly Linked List Operations
- **Head**: Points to the MRU (Most Recently Used) item
- **Tail**: Points to the LRU (Least Recently Used) item
- **Node Structure**:
  ```c
  typedef struct cache_element {
      char* data;               // Cached response data
      int len;                  // Length of data
      char* url;                // Request URL (key)
      time_t lru_time_track;    // Last access time
      struct cache_element* prev;
      struct cache_element* next;
  } cache_element;
  ```

### 6. Thread Safety

- All cache operations are protected by `cache_lock` mutex
- Ensures thread-safe access to shared cache data
- Prevents race conditions in a multi-threaded environment

### 7. Error Handling and Logging

- **Error Logging**: `LOG_ERROR` macro for error conditions
- **Debug Logging**: `LOG_DEBUG` for debugging information
- **Safe Logging**: `safe_log()` for thread-safe logging
- **Error Responses**: `send_error_response()` for HTTP error responses

### 8. Cache Statistics

The system maintains statistics:
- `total_hits`: Number of successful cache lookups
- `total_misses`: Number of cache misses
- `current_size`: Current size of cached data
- `max_size`: Maximum allowed cache size

### 9. Cleanup

- `cache_cleanup()`: Properly frees all allocated memory
- Called during server shutdown
- Ensures no memory leaks

## Example Scenarios

### Scenario 1: First Request (Cache Miss)
1. Client requests `/page1`
2. Cache miss → Forward to backend
3. Backend responds with page content
4. Response is cached
5. Content sent to client

### Scenario 2: Subsequent Request (Cache Hit)
1. Client requests `/page1` again
2. Cache hit → Retrieve from cache
3. Move item to front (MRU)
4. Send cached content to client

### Scenario 3: Cache Full
1. Cache is full (total_cache_size ≥ MAX_CACHE_SIZE)
2. New request comes in
3. LRU items are evicted until there's enough space
4. New item is added to the cache

### Scenario 4: Invalidation
1. Admin wants to remove stale content
2. `cache_invalidate(url)` is called
3. Item is removed from cache if it exists
4. Future requests will get fresh content from backend
