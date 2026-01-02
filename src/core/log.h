#pragma once
#include <stdbool.h>
#include <stdarg.h>

void log_info(const char *component, const char *message);
void log_ok(const char *component, const char *message);
void log_error(const char *component, const char *message);

void log_infof(const char *component, const char *fmt, ...);
void log_okf(const char *component, const char *fmt, ...);
void log_errorf(const char *component, const char *fmt, ...);

void log_enable_serial(bool on);
