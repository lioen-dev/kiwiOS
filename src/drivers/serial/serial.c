#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stddef.h>
#include "drivers/serial/serial.h"
#include "arch/x86/io.h"

#define COM1 0x3F8

static inline void outb_u16(uint16_t port, uint8_t val) { outb(port, val); }
static inline uint8_t inb_u16(uint16_t port) { return inb(port); }

static int serial_transmit_empty(void) {
    return inb_u16(COM1 + 5) & 0x20; // LSR bit 5
}

bool serial_init(void) {
    // Disable interrupts
    outb_u16(COM1 + 1, 0x00);

    // Enable DLAB (set baud rate divisor)
    outb_u16(COM1 + 3, 0x80);

    // Set divisor to 3 (38400 baud if base is 115200)
    outb_u16(COM1 + 0, 0x03);
    outb_u16(COM1 + 1, 0x00);

    // 8 bits, no parity, one stop bit
    outb_u16(COM1 + 3, 0x03);

    // Enable FIFO, clear them, 14-byte threshold
    outb_u16(COM1 + 2, 0xC7);

    // IRQs enabled, RTS/DSR set
    outb_u16(COM1 + 4, 0x0B);

    // Quick loopback test
    outb_u16(COM1 + 4, 0x1E);      // loopback
    outb_u16(COM1 + 0, 0xAE);
    if (inb_u16(COM1 + 0) != 0xAE) {
        outb_u16(COM1 + 4, 0x0F);
        return false;
    }

    // Normal operation
    outb_u16(COM1 + 4, 0x0F);
    return true;
}

void serial_putc(char c) {
    if (c == '\n') serial_putc('\r');

    while (!serial_transmit_empty()) {
        // spin
    }
    outb_u16(COM1 + 0, (uint8_t)c);
}

void serial_write(const char *s) {
    if (!s) return;
    while (*s) serial_putc(*s++);
}

/* ---------- printf-like support (matches your kvprintf specifiers) ---------- */

static void s_print_unsigned(unsigned long long value, int base, bool uppercase) {
    char buf[32];
    const char *digits = uppercase ? "0123456789ABCDEF" : "0123456789abcdef";
    int pos = 0;

    if (value == 0) {
        serial_putc('0');
        return;
    }

    while (value && pos < (int)sizeof(buf)) {
        buf[pos++] = digits[value % (unsigned)base];
        value /= (unsigned)base;
    }

    while (pos--) serial_putc(buf[pos]);
}

static void s_print_signed(long long value) {
    if (value < 0) {
        serial_putc('-');
        s_print_unsigned((unsigned long long)(-value), 10, false);
    } else {
        s_print_unsigned((unsigned long long)value, 10, false);
    }
}

void serial_kvprintf(const char *fmt, va_list args) {
    for (const char *p = fmt; *p; ++p) {
        if (*p != '%') {
            serial_putc(*p);
            continue;
        }

        ++p;
        switch (*p) {
            case 's': {
                const char *s = va_arg(args, const char *);
                if (!s) s = "(null)";
                while (*s) serial_putc(*s++);
                break;
            }
            case 'c': {
                char c = (char)va_arg(args, int);
                serial_putc(c);
                break;
            }
            case 'd':
            case 'i': {
                int val = va_arg(args, int);
                s_print_signed(val);
                break;
            }
            case 'u': {
                unsigned int val = va_arg(args, unsigned int);
                s_print_unsigned(val, 10, false);
                break;
            }
            case 'x': {
                unsigned int val = va_arg(args, unsigned int);
                s_print_unsigned(val, 16, false);
                break;
            }
            case 'X': {
                unsigned int val = va_arg(args, unsigned int);
                s_print_unsigned(val, 16, true);
                break;
            }
            case 'p': {
                uint64_t val = (uint64_t)(uintptr_t)va_arg(args, void *);
                serial_write("0x");
                s_print_unsigned(val, 16, false);
                break;
            }
            case '%': {
                serial_putc('%');
                break;
            }
            default:
                serial_putc('%');
                serial_putc(*p);
                break;
        }
    }
}

void serial_kprintf(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    serial_kvprintf(fmt, args);
    va_end(args);
}

void serial_print_hex(uint64_t num) {
    serial_write("0x");
    static const char hex[] = "0123456789ABCDEF";
    char buf[17]; buf[16] = '\0';
    for (int i = 15; i >= 0; i--) { buf[i] = hex[num & 0xF]; num >>= 4; }
    serial_write(buf);
}