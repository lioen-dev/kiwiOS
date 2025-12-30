#ifndef ARCH_X86_CONTEXT_H
#define ARCH_X86_CONTEXT_H

#include <stdint.h>

typedef struct {
    uint64_t r15;
    uint64_t r14;
    uint64_t r13;
    uint64_t r12;
    uint64_t rbx;
    uint64_t rbp;
    uint64_t rsp;
} arch_context_t;

void arch_context_switch(arch_context_t *old_ctx, const arch_context_t *new_ctx);

#endif // ARCH_X86_CONTEXT_H
