#ifndef ARCH_X86_IDT_H
#define ARCH_X86_IDT_H

#include <stdint.h>
#include <stdbool.h>

void init_idt(void);
void idt_enable_serial(bool on);

#endif // ARCH_X86_IDT_H
