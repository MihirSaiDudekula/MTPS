#ifndef LOGGER_H
#define LOGGER_H

#include <stdbool.h>
#include <stdio.h>
#include <pthread.h>

// Thread-safe logging
void safe_log(const char* format, ...);

// Error logging macro
#define LOG_ERROR(msg) fprintf(stderr, "ERROR: %s: %s\n", msg, strerror(errno))

// Debug logging macro (only logs if debug mode is enabled)
#define LOG_DEBUG(msg) if (config.debug_mode) fprintf(stderr, "DEBUG: %s\n", msg)

#endif // LOGGER_H
