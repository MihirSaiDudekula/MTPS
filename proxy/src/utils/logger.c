#include "../../include/logger.h"
#include "../../include/config.h"
#include <stdarg.h>
#include <string.h>
#include <errno.h>

void safe_log(const char* format, ...) {
    static pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;
    
    pthread_mutex_lock(&log_mutex);
    
    va_list args;
    va_start(args, format);
    vprintf(format, args);
    printf("\n");
    fflush(stdout);
    va_end(args);
    
    pthread_mutex_unlock(&log_mutex);
}
