#include "arch/x86/context.h"

// Simple context switch that saves callee-saved registers and the stack
// pointer for the outgoing thread, then restores them for the incoming one.
__attribute__((naked)) void arch_context_switch(arch_context_t *old_ctx, const arch_context_t *new_ctx) {
    asm volatile(
        "mov %0, %%rax\n\t"
        "mov %1, %%rdx\n\t"
        "mov %%r15, 0x00(%%rax)\n\t"
        "mov %%r14, 0x08(%%rax)\n\t"
        "mov %%r13, 0x10(%%rax)\n\t"
        "mov %%r12, 0x18(%%rax)\n\t"
        "mov %%rbx, 0x20(%%rax)\n\t"
        "mov %%rbp, 0x28(%%rax)\n\t"
        "mov %%rsp, 0x30(%%rax)\n\t"
        "mov 0x00(%%rdx), %%r15\n\t"
        "mov 0x08(%%rdx), %%r14\n\t"
        "mov 0x10(%%rdx), %%r13\n\t"
        "mov 0x18(%%rdx), %%r12\n\t"
        "mov 0x20(%%rdx), %%rbx\n\t"
        "mov 0x28(%%rdx), %%rbp\n\t"
        "mov 0x30(%%rdx), %%rsp\n\t"
        "ret\n\t"
        :
        : "r"(old_ctx), "r"(new_ctx)
        : "rax", "rdx", "memory"
    );
}
