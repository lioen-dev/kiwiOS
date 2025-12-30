#ifndef ARCH_X86_IDT_H
#define ARCH_X86_IDT_H

#include <stdbool.h>
#include <stdint.h>

struct interrupt_frame {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t int_no, error_code;
    uint64_t rip, cs, rflags, rsp, ss;
} __attribute__((packed));

typedef void (*interrupt_handler_t)(struct interrupt_frame *frame);

void interrupts_init(void);
void interrupts_register_handler(uint8_t vector, interrupt_handler_t handler);
void interrupts_enable(void);
void interrupts_disable(void);
bool interrupts_apic_enabled(void);
void interrupts_send_eoi(uint8_t vector);

// Backwards compatibility for existing init_idt callers.
static inline void init_idt(void) { interrupts_init(); }

#endif // ARCH_X86_IDT_H
