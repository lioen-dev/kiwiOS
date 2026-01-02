#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>

bool serial_init(void);          // returns true if it looks alive
void serial_putc(char c);
void serial_write(const char *s);

// printf-style helpers for serial
void serial_kvprintf(const char *fmt, va_list args);
void serial_kprintf(const char *fmt, ...);
void serial_print_hex(uint64_t num);