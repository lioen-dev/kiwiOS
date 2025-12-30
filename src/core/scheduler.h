#ifndef CORE_SCHEDULER_H
#define CORE_SCHEDULER_H

#include <stddef.h>
#include <stdint.h>
#include "arch/x86/context.h"
#include "arch/x86/idt.h"

typedef enum {
    THREAD_UNUSED = 0,
    THREAD_READY,
    THREAD_RUNNING,
    THREAD_BLOCKED,
    THREAD_DEAD
} thread_state_t;

typedef struct thread {
    int id;
    const char *name;
    int priority;
    thread_state_t state;
    arch_context_t context;
    uint8_t *kstack_base;
    size_t kstack_size;
    void (*entry)(void *arg);
    void *arg;
} thread_t;

void scheduler_init(void);
thread_t *scheduler_current(void);
thread_t *scheduler_create(const char *name, void (*entry)(void *), void *arg, size_t stack_size, int priority);
void scheduler_on_tick(const struct interrupt_frame *frame);
void scheduler_yield(void);

#endif // CORE_SCHEDULER_H
