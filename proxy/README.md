# MTPS Proxy Server

A high-performance, multi-threaded HTTP proxy server with caching capabilities.

## Features

- Multi-threaded architecture for handling multiple clients concurrently
- LRU (Least Recently Used) caching with configurable size limits
- Support for HTTP/1.1 requests
- Configurable backend server and port
- Detailed logging and statistics
- Graceful shutdown on SIGINT/SIGTERM

## Building

1. Ensure you have GCC and Make installed
2. Clone the repository
3. Run `make` to build the project
4. The binary will be created in the `bin` directory

## Usage

```
./bin/proxy [options]

Options:
  -p, --port PORT         Port to listen on (default: 8080)
  -h, --host HOST         Backend host to forward requests to (default: localhost)
  -t, --target-port PORT  Backend port to forward requests to (default: 3000)
  -c, --cache-size SIZE   Maximum cache size in MB (default: 200)
  -e, --element-size SIZE Maximum size of a single cache element in MB (default: 10)
  -m, --max-clients NUM   Maximum number of concurrent clients (default: 10)
  -d, --debug             Enable debug logging
  --help                  Show this help message
```

## Project Structure

```
proxy/
├── bin/                  # Compiled binary
├── include/              # Header files
│   ├── cache.h           # Cache interface
│   ├── config.h          # Configuration structures
│   ├── logger.h          # Logging utilities
│   ├── http/             # HTTP protocol handling
│   │   ├── request.h     # HTTP request parsing
│   │   └── response.h    # HTTP response handling
│   └── network/          # Network communication
│       └── connection.h  # Socket and connection management
├── src/                  # Source files
│   ├── main.c            # Entry point
│   ├── cache/            # Cache implementation
│   ├── http/             # HTTP implementation
│   ├── network/          # Network implementation
│   └── utils/            # Utility functions
└── Makefile              # Build configuration
```

## Cache Implementation

The proxy implements an LRU (Least Recently Used) cache with the following features:

- Fixed maximum size
- O(1) insert/delete operations
- Thread-safe operations
- Automatic eviction of least recently used items when the cache is full

See [docs/CACHE_IMPLEMENTATION.md](docs/CACHE_IMPLEMENTATION.md) for more details.

## License

This project is licensed under the MIT License - see the LICENSE file for details.
