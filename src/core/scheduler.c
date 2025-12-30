#include "core/scheduler.h"
#include "arch/x86/idt.h"
#include "core/log.h"
#include "memory/heap.h"
#include "memory/pmm.h"
#include "libc/string.h"

#define MAX_THREADS 16
#define DEFAULT_STACK_PAGES 4

static thread_t threads[MAX_THREADS];
static thread_t *current_thread = NULL;
static size_t thread_count = 0;
static volatile int reschedule_requested = 0;

static thread_t *allocate_thread_slot(void) {
    for (int i = 0; i < MAX_THREADS; i++) {
        if (threads[i].state == THREAD_UNUSED || threads[i].state == THREAD_DEAD) {
            memset(&threads[i], 0, sizeof(thread_t));
            threads[i].state = THREAD_READY;
            threads[i].id = i;
            return &threads[i];
        }
    }
    return NULL;
}

static void thread_trampoline(void) {
    thread_t *self = scheduler_current();
    if (self && self->entry) {
        self->entry(self->arg);
    }

    self->state = THREAD_DEAD;
    scheduler_yield();

    // Should never return; halt safely if we do.
    for (;;) {
        asm volatile("hlt");
    }
}

static thread_t *next_runnable(void) {
    if (thread_count <= 1) {
        return current_thread;
    }

    int start = current_thread ? (current_thread->id + 1) % MAX_THREADS : 0;
    for (int i = 0; i < MAX_THREADS; i++) {
        int idx = (start + i) % MAX_THREADS;
        if (threads[idx].state == THREAD_READY) {
            return &threads[idx];
        }
    }

    return current_thread;
}

void scheduler_init(void) {
    memset(threads, 0, sizeof(threads));
    current_thread = &threads[0];
    current_thread->id = 0;
    current_thread->name = "bootstrap";
    current_thread->state = THREAD_RUNNING;
    current_thread->priority = 0;

    uint64_t rsp = 0;
    asm volatile("mov %%rsp, %0" : "=r"(rsp));
    current_thread->context.rsp = rsp;
    thread_count = 1;

    log_ok("sched", "Scheduler initialized with bootstrap thread");
}

thread_t *scheduler_current(void) {
    return current_thread;
}

thread_t *scheduler_create(const char *name, void (*entry)(void *), void *arg, size_t stack_size, int priority) {
    thread_t *t = allocate_thread_slot();
    if (!t) {
        log_error("sched", "No available thread slots");
        return NULL;
    }

    size_t pages = (stack_size + PAGE_SIZE - 1) / PAGE_SIZE;
    if (pages == 0) {
        pages = DEFAULT_STACK_PAGES;
    }

    t->kstack_size = pages * PAGE_SIZE;
    t->kstack_base = kmalloc(t->kstack_size);
    if (!t->kstack_base) {
        log_error("sched", "Failed to allocate kernel stack");
        t->state = THREAD_UNUSED;
        return NULL;
    }

    uint64_t stack_top = (uint64_t)(t->kstack_base + t->kstack_size);
    stack_top &= ~0xFULL; // Align to 16 bytes.

    // Place the trampoline as the first return address for when the context is loaded.
    stack_top -= sizeof(uint64_t);
    *((uint64_t *)stack_top) = (uint64_t)thread_trampoline;

    memset(&t->context, 0, sizeof(t->context));
    t->context.rsp = stack_top;
    t->name = name ? name : "thread";
    t->entry = entry;
    t->arg = arg;
    t->priority = priority;
    t->state = THREAD_READY;

    thread_count++;
    log_info("sched", "Thread '%s' created on slot %d", t->name, t->id);
    return t;
}

void scheduler_on_tick(const struct interrupt_frame *frame) {
    (void)frame;
    reschedule_requested = 1;
}

void scheduler_yield(void) {
    if (!current_thread) {
        return;
    }

    interrupts_disable();

    thread_t *next = next_runnable();
    int requested = reschedule_requested;
    reschedule_requested = 0;

    if (!next || (!requested && next == current_thread)) {
        interrupts_enable();
        return;
    }

    thread_t *prev = current_thread;
    next->state = THREAD_RUNNING;
    current_thread = next;
    if (prev->state == THREAD_RUNNING) {
        prev->state = THREAD_READY;
    }

    interrupts_enable();
    arch_context_switch(&prev->context, &next->context);
}
