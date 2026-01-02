#include "core/log.h"
#include "libc/stdio.h"
#include "drivers/serial/serial.h"
#include <stdarg.h>
#include <stdbool.h>

static bool g_serial_enabled = false;
void log_enable_serial(bool on) { g_serial_enabled = on; }

static void log_with_level(const char *level, const char *component, const char *message) {
    // Screen
    kprintf("[%s] [%s] %s\n", level, component, message);

    // Serial
    if (g_serial_enabled) {
        serial_kprintf("[%s] [%s] %s\n", level, component, message);
    }
}

static void vlog_with_level(const char *level, const char *component, const char *fmt, va_list args) {
    // We must copy args if we want to print twice
    va_list args_copy;
    va_copy(args_copy, args);

    // Screen
    kprintf("[%s] [%s] ", level, component);
    kvprintf(fmt, args);
    kprintf("\n");

    // Serial
    if (g_serial_enabled) {
        serial_kprintf("[%s] [%s] ", level, component);
        serial_kvprintf(fmt, args_copy);
        serial_kprintf("\n");
    }

    va_end(args_copy);
}

void log_info(const char *component, const char *message) { log_with_level("INFO", component, message); }
void log_ok(const char *component, const char *message)   { log_with_level(" OK ", component, message); }
void log_error(const char *component, const char *message){ log_with_level("ERR ", component, message); }

void log_infof(const char *component, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vlog_with_level("INFO", component, fmt, args);
    va_end(args);
}

void log_okf(const char *component, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vlog_with_level(" OK ", component, fmt, args);
    va_end(args);
}

void log_errorf(const char *component, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vlog_with_level("ERR ", component, fmt, args);
    va_end(args);
}
