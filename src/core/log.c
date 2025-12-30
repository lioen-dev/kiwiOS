#include "core/log.h"
#include <stdarg.h>
#include "libc/stdio.h"

static void log_with_level(const char *level, const char *component, const char *fmt, va_list args) {
    kprintf("[%s] [%s] ", level, component);
    kvprintf(fmt, args);
    kprintf("\n");
}

void log_info(const char *component, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    log_with_level("INFO", component, fmt, args);
    va_end(args);
}

void log_ok(const char *component, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    log_with_level(" OK ", component, fmt, args);
    va_end(args);
}

void log_error(const char *component, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    log_with_level("ERR ", component, fmt, args);
    va_end(args);
}
