structs i care about:
ProxyConfig
CacheStats
http_request
cache_element

funcs i care about:
parse_http_request
validate url and headers
parse_http_request
connect_to_server

A macro in C (short for macro definition) is a way to define a shortcut or code pattern that gets replaced by the preprocessor before the actual compilation happens

| Field              | Type     | Description                                        |
| ------------------ | -------- | -------------------------------------------------- |
| `port`             | `int`    | Port number the proxy listens on (e.g., 8080)      |
| `target_host`      | `char*`  | Hostname of the backend server (e.g., "localhost") |
| `target_port`      | `int`    | Port of the backend server (e.g., 3000)            |
| `max_cache_size`   | `size_t` | Maximum total cache size (e.g., 200 MB)            |
| `max_element_size` | `size_t` | Max size of a single cached item (e.g., 10 KB)     |
| `max_clients`      | `int`    | How many clients can connect at once               |
| `debug_mode`       | `bool`   | Whether to print debug logs (`true` or `false`)    |


In C, we store strings as char* (a pointer to char)

so each proxy server has the properties:

Port to listen on
Backend server hostname
Backend server port
Maximum cache size in bytes
Maximum size of a single cached item
Maximum concurrent clients
Enable debug logging or not


In C, a variadic function can take a flexible number of arguments — which is useful when you don't know ahead of time how many values will be passed in
va_list, va_start, va_end, and vprintf are part of <stdarg.h> — the C standard library for handling variable arguments.
vprintf is like printf, but it takes a va_list instead of a variable number of arguments directl

When you call:

c
Copy
Edit
my_log("You are %d years old", 30);
Here's what happens:

my_log receives "You are %d years old" as format, and 30 as part of the ....

va_start(args, format) initializes args so you can extract the variadic arguments.

vprintf(format, args) uses the format string and reads from args to print the full string.

va_end(args) tells the compiler you're done reading the variable arguments.

http request breakdown:
struct http_request {
    char method[16];         // e.g., "GET", "POST"
    char url[2048];          // Full URL requested
    char host[256];          // Host header from the request
    char* body;              // Pointer to the body of the request (e.g., POST data)
    int content_length;      // Length of the body in bytes
    char content_type[128];  // MIME type of the body, e.g., "application/json"
};
| Field            | Type         | Description                                                                                         |
| ---------------- | ------------ | --------------------------------------------------------------------------------------------------- |
| `method`         | `char[16]`   | Stores the HTTP method (e.g., `"GET"`, `"POST"`). It's a fixed-size array of characters.            |
| `url`            | `char[2048]` | Stores the requested URL. 2048 is a common practical maximum length for URLs.                       |
| `host`           | `char[256]`  | Stores the `Host:` header from the HTTP request. 255 characters is a safe limit for most hostnames. |
| `body`           | `char*`      | Pointer to the request body (e.g., for `POST`, `PUT`). Can be `NULL` for methods like `GET`.        |
| `content_length` | `int`        | The number of bytes in the `body`. Usually from the `Content-Length` header.                        |
| `content_type`   | `char[128]`  | The `Content-Type` header (e.g., `"application/json"`, `"text/html"`).                              |

struct http_request req;

strcpy(req.method, "POST");
strcpy(req.url, "/api/login");
strcpy(req.host, "example.com");
req.body = strdup("username=admin&password=1234");
req.content_length = strlen(req.body);
strcpy(req.content_type, "application/x-www-form-urlencoded");


cache working:
cache_element* e = malloc(sizeof(cache_element));
e->url = strdup("http://example.com");
e->data = strdup("<html>...</html>");
e->len = strlen(e->data);
e->lru_time_track = time(NULL);
e->next = NULL;  // This will be linked later


Absolutely — this code snippet is using **POSIX threads (`pthread`)** to support **multithreading** in a server (probably a proxy or web server). Here's a quick breakdown of each line:

---

### \`\`\`c

pthread\_t tid\[MAX\_CLIENTS];

````

- Creates an **array of thread IDs**.
- Each `pthread_t` represents a separate client-handling thread.
- The array size is limited by `MAX_CLIENTS`, meaning the server can handle up to that many clients concurrently.

---

### ```c
pthread_mutex_t cache_lock;
````

* A **mutex** (mutual exclusion lock) to **protect shared access** to the cache (like `cache_head` or `total_cache_size`).
* Only one thread can hold the lock at a time — this prevents **race conditions** when multiple threads read/write the cache.

---

### \`\`\`c

sem\_t connection\_semaphore;

````

- A **semaphore** (from `<semaphore.h>`) used to **limit the number of concurrent client connections**.
- Before accepting a new client, a thread must `sem_wait(&connection_semaphore)`. When it finishes, it `sem_post(...)`.
- Think of it as a counter: when it hits 0, no more clients can be handled until a thread releases a slot.

---

### ```c
cache_element* cache_head = NULL;
````

* Pointer to the **head of a linked list** that stores cached content.
* Shared by all threads — hence the need for `cache_lock`.

---

### \`\`\`c

int total\_cache\_size = 0;

```

- Tracks the **current size of the cache in bytes**.
- Also shared across threads, and must be protected by `cache_lock`.

---

### 🧠 In summary:

| Component               | Purpose                                               |
|------------------------|-------------------------------------------------------|
| `pthread_t tid[]`      | Stores thread IDs for client connections             |
| `pthread_mutex_t`      | Ensures one thread at a time accesses shared cache   |
| `sem_t`                | Limits how many clients are handled concurrently     |
| `cache_head` / `total_cache_size` | The actual cache data shared among threads |

This is a typical setup for a **multithreaded, concurrent server** that caches responses and serves multiple clients safely.

Let me know if you want a full-threaded server example using this structure!
```
Great — you're looking at this line:

```c
setsockopt(server_socket, SOL_SOCKET, SO_RCVTIMEO, 
           (const char*)&timeout, sizeof(timeout));
```

This line configures a **timeout for receiving data** on a socket. Let’s break it down step by step so it’s 100% clear:

---

## 🧠 What does `setsockopt` do?

The function `setsockopt()` allows you to **configure socket behavior** — like setting timeouts, reusing ports, enabling keepalive, etc.

---

## ✅ What this specific line does

It sets the **receive timeout** (`SO_RCVTIMEO`) for the socket `server_socket`.

### Meaning:

If a `recv()` or `read()` on that socket takes longer than the timeout, it will **fail with a timeout error** instead of hanging forever.

---

### 🔍 Breakdown of parameters:

```c
setsockopt(
    server_socket,         // the socket file descriptor
    SOL_SOCKET,            // level: "socket-level" option
    SO_RCVTIMEO,           // option name: set receive timeout
    (const char*)&timeout, // pointer to the timeout value
    sizeof(timeout)        // size of the value
);
```

* **`SOL_SOCKET`**: Tells the kernel we're setting a general socket option.
* **`SO_RCVTIMEO`**: The specific option — sets how long `recv()` will wait before timing out.
* **`timeout`**: A `struct timeval` (with seconds and microseconds).
* **`sizeof(timeout)`**: The kernel needs to know how big the timeout structure is.

---

### 📦 `timeout` must look like:

```c
struct timeval timeout;
timeout.tv_sec = 2;     // 2 seconds
timeout.tv_usec = 500000; // 0.5 seconds
```

This sets the timeout to **2.5 seconds**.

---

### 🔁 What happens if the timeout hits?

If the socket is waiting for data using `recv()` and **no data arrives before the timeout**, the function will:

* Return `-1`
* Set `errno = EAGAIN` or `EWOULDBLOCK`

This lets your program avoid getting stuck forever.

---

### 🧪 Example use:

```c
char buffer[1024];
int n = recv(server_socket, buffer, sizeof(buffer), 0);
if (n < 0 && errno == EAGAIN) {
    printf("Receive timed out!\n");
}
```

---

### 🧵 Related option

There's also:

```c
SO_SNDTIMEO
```

Which does the same thing, but for **sending** data (`send()`/`write()`).

---

### 📝 Summary

This line:

```c
setsockopt(server_socket, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));
```

means:

> “Set a timeout on how long `recv()` should wait for data on `server_socket`. If the wait takes too long, fail instead of hanging.”

---

Big Picture: What are we trying to do?
You’re building a multi-threaded HTTP proxy server in C.

Here’s the flow:

🧍 A client (e.g. browser) connects to your server (over a TCP socket).

🖥️ Your server accepts the connection using accept().

🔄 It spawns a new thread to handle that client.

📦 That thread reads the request, checks cache, talks to the target server if needed, and sends a response.

✅ Then it closes the connection and exits.