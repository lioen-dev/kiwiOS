#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "arch/x86/idt.h"
#include "arch/x86/io.h"
#include "core/console.h"
#include "core/log.h"
#include "core/scheduler.h"
#include "libc/string.h"
#include "memory/hhdm.h"

struct idt_entry {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t ist;
    uint8_t type_attr;
    uint16_t offset_mid;
    uint32_t offset_high;
    uint32_t zero;
} __attribute__((packed));

struct idt_ptr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

#define IDT_ENTRIES 256
#define PIC1_COMMAND 0x20
#define PIC1_DATA 0x21
#define PIC2_COMMAND 0xA0
#define PIC2_DATA 0xA1
#define PIC_EOI 0x20

#define IA32_APIC_BASE_MSR 0x1B
#define LAPIC_ENABLE (1ULL << 11)
#define LAPIC_SPURIOUS_VECTOR 0xFF

static struct idt_entry idt[IDT_ENTRIES];
static struct idt_ptr idtr;
static interrupt_handler_t interrupt_handlers[IDT_ENTRIES];

static volatile uint32_t *lapic_regs = NULL;
static bool lapic_enabled = false;

static const char *exception_messages[] = {
    "Division By Zero", "Debug", "Non Maskable Interrupt", "Breakpoint",
    "Overflow", "Bound Range Exceeded", "Invalid Opcode", "Device Not Available",
    "Double Fault", "Coprocessor Segment Overrun", "Invalid TSS", "Segment Not Present",
    "Stack-Segment Fault", "General Protection Fault", "Page Fault", "Reserved",
    "x87 Floating-Point Exception", "Alignment Check", "Machine Check", "SIMD Floating-Point Exception",
    "Virtualization Exception", "Control Protection Exception", "Reserved", "Reserved",
    "Reserved", "Reserved", "Reserved", "Reserved", "Hypervisor Injection Exception",
    "VMM Communication Exception", "Security Exception", "Reserved"
};

static inline uint64_t rdmsr(uint32_t msr) {
    uint32_t low, high;
    asm volatile ("rdmsr" : "=a"(low), "=d"(high) : "c"(msr));
    return ((uint64_t)high << 32) | low;
}

static inline void wrmsr(uint32_t msr, uint64_t value) {
    uint32_t low = (uint32_t)value;
    uint32_t high = (uint32_t)(value >> 32);
    asm volatile ("wrmsr" : : "c"(msr), "a"(low), "d"(high));
}

static inline uint32_t lapic_read(uint32_t reg) {
    return lapic_regs[reg / 4];
}

static inline void lapic_write(uint32_t reg, uint32_t value) {
    lapic_regs[reg / 4] = value;
}

static void pic_remap(void) {
    outb(PIC1_COMMAND, 0x11);
    outb(PIC2_COMMAND, 0x11);
    outb(PIC1_DATA, 0x20);
    outb(PIC2_DATA, 0x28);
    outb(PIC1_DATA, 0x04);
    outb(PIC2_DATA, 0x02);
    outb(PIC1_DATA, 0x01);
    outb(PIC2_DATA, 0x01);

    // Mask everything by default, unmask timer on PIC1.
    outb(PIC1_DATA, 0xFE);
    outb(PIC2_DATA, 0xFF);
}

void interrupts_send_eoi(uint8_t vector) {
    if (lapic_enabled) {
        lapic_write(0xB0, 0);
        return;
    }

    if (vector >= 40) {
        outb(PIC2_COMMAND, PIC_EOI);
    }
    outb(PIC1_COMMAND, PIC_EOI);
}

static void try_enable_apic(void) {
    uint32_t eax, ebx, ecx, edx;
    eax = 1;
    asm volatile ("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(eax));

    if (!(edx & (1 << 9))) {
        log_info("apic", "Local APIC not reported by CPUID; continuing with PIC");
        return;
    }

    uint64_t apic_base = rdmsr(IA32_APIC_BASE_MSR) | LAPIC_ENABLE;
    wrmsr(IA32_APIC_BASE_MSR, apic_base);

    uint64_t lapic_phys = apic_base & 0xFFFFF000;
    lapic_regs = (uint32_t *)hhdm_phys_to_virt(lapic_phys);
    lapic_enabled = (lapic_regs != NULL);

    if (!lapic_enabled) {
        log_error("apic", "Failed to map LAPIC base; falling back to PIC");
        return;
    }

    uint32_t svr = lapic_read(0xF0);
    lapic_write(0xF0, (svr & 0xFFFFFF00) | LAPIC_SPURIOUS_VECTOR | (1 << 8));
    lapic_write(0xD0, 0); // Task priority to 0 to accept all interrupts.

    log_ok("apic", "Local APIC enabled with spurious vector 0xFF");
}

static inline void dump_frame(struct interrupt_frame *frame) {
    uint32_t old_fg, old_bg;
    console_get_colors(&old_fg, &old_bg);
    console_set_colors(0x00FFFFFF, 0x00913030);
    console_reset_scrollback();
    console_clear_outputs();
    console_render_visible();

    print(NULL, "\n  :3 uh oh, KERNEL PANIC!\n");
    print(NULL, "===========================\n\n");

    print(NULL, "Exception: ");
    if (frame->int_no < 32) {
        print(NULL, exception_messages[frame->int_no]);
    } else {
        print(NULL, "Unknown Exception");
    }
    print(NULL, "\n");

    print(NULL, "Exception Number: "); print_hex(NULL, frame->int_no); print(NULL, "\n");
    print(NULL, "Error Code: ");       print_hex(NULL, frame->error_code); print(NULL, "\n\n");

    print(NULL, "RIP: "); print_hex(NULL, frame->rip);    print(NULL, "   CS: ");    print_hex(NULL, frame->cs);     print(NULL, "\n");
    print(NULL, "RSP: "); print_hex(NULL, frame->rsp);    print(NULL, "   SS: ");    print_hex(NULL, frame->ss);     print(NULL, "\n");
    print(NULL, "RFLAGS: "); print_hex(NULL, frame->rflags);                        print(NULL, "\n");

    uint64_t cr2 = 0;
    asm volatile("mov %%cr2, %0" : "=r"(cr2));
    print(NULL, "CR2: "); print_hex(NULL, cr2); print(NULL, "\n");

    print(NULL, "RAX: "); print_hex(NULL, frame->rax);    print(NULL, "   RBX: ");   print_hex(NULL, frame->rbx);    print(NULL, "\n");
    print(NULL, "RCX: "); print_hex(NULL, frame->rcx);    print(NULL, "   RDX: ");   print_hex(NULL, frame->rdx);    print(NULL, "\n");
    print(NULL, "RSI: "); print_hex(NULL, frame->rsi);    print(NULL, "   RDI: ");   print_hex(NULL, frame->rdi);    print(NULL, "\n");
    print(NULL, "RBP: "); print_hex(NULL, frame->rbp);    print(NULL, "   R8 : ");   print_hex(NULL, frame->r8);     print(NULL, "\n");
    print(NULL, "R9 : "); print_hex(NULL, frame->r9);     print(NULL, "   R10: ");   print_hex(NULL, frame->r10);    print(NULL, "\n");
    print(NULL, "R11: "); print_hex(NULL, frame->r11);    print(NULL, "   R12: ");   print_hex(NULL, frame->r12);    print(NULL, "\n");
    print(NULL, "R13: "); print_hex(NULL, frame->r13);    print(NULL, "   R14: ");   print_hex(NULL, frame->r14);    print(NULL, "\n");
    print(NULL, "R15: "); print_hex(NULL, frame->r15);    print(NULL, "\n");

    print(NULL, "\nSystem Halted.\n");

    console_set_colors(old_fg, old_bg);
}

__attribute__((noreturn)) static void panic_halt_forever(void) {
    asm volatile("cli");
    for (;;) asm volatile("hlt");
}

static void fault_handler(struct interrupt_frame *frame) {
    dump_frame(frame);
    panic_halt_forever();
}

static void default_irq_handler(struct interrupt_frame *frame) {
    log_info("irq", "Unhandled IRQ vector %u", (unsigned)frame->int_no);
}

__attribute__((used)) static void dispatch_interrupt(struct interrupt_frame *frame) {
    if (frame->int_no < 32) {
        fault_handler(frame);
        return;
    }

    interrupt_handler_t handler = interrupt_handlers[frame->int_no];
    if (handler) {
        handler(frame);
    } else {
        default_irq_handler(frame);
    }

    if (frame->int_no >= 32 && frame->int_no < 48) {
        interrupts_send_eoi((uint8_t)frame->int_no);
    }
}

static void timer_handler(struct interrupt_frame *frame) {
    scheduler_on_tick(frame);
}

extern void isr_common_stub(void);

#define DEFINE_ISR_NOERR(num) \
    __attribute__((naked)) void isr_##num(void) { \
        asm volatile( \
            "push $0\n" \
            "push $" #num "\n" \
            "jmp isr_common_stub\n" \
        ); \
    }

#define DEFINE_ISR_ERR(num) \
    __attribute__((naked)) void isr_##num(void) { \
        asm volatile( \
            "push $" #num "\n" \
            "jmp isr_common_stub\n" \
        ); \
    }

// Exceptions 0-31
DEFINE_ISR_NOERR(0)  DEFINE_ISR_NOERR(1)  DEFINE_ISR_NOERR(2)  DEFINE_ISR_NOERR(3)
DEFINE_ISR_NOERR(4)  DEFINE_ISR_NOERR(5)  DEFINE_ISR_NOERR(6)  DEFINE_ISR_NOERR(7)
DEFINE_ISR_ERR(8)    DEFINE_ISR_NOERR(9)  DEFINE_ISR_ERR(10)   DEFINE_ISR_ERR(11)
DEFINE_ISR_ERR(12)   DEFINE_ISR_ERR(13)   DEFINE_ISR_ERR(14)   DEFINE_ISR_NOERR(15)
DEFINE_ISR_NOERR(16) DEFINE_ISR_ERR(17)   DEFINE_ISR_NOERR(18) DEFINE_ISR_NOERR(19)
DEFINE_ISR_NOERR(20) DEFINE_ISR_ERR(21)   DEFINE_ISR_NOERR(22) DEFINE_ISR_NOERR(23)
DEFINE_ISR_NOERR(24) DEFINE_ISR_NOERR(25) DEFINE_ISR_NOERR(26) DEFINE_ISR_NOERR(27)
DEFINE_ISR_NOERR(28) DEFINE_ISR_ERR(29)   DEFINE_ISR_NOERR(30) DEFINE_ISR_NOERR(31)

// IRQ 0-15 (vectors 32-47)
DEFINE_ISR_NOERR(32) DEFINE_ISR_NOERR(33) DEFINE_ISR_NOERR(34) DEFINE_ISR_NOERR(35)
DEFINE_ISR_NOERR(36) DEFINE_ISR_NOERR(37) DEFINE_ISR_NOERR(38) DEFINE_ISR_NOERR(39)
DEFINE_ISR_NOERR(40) DEFINE_ISR_NOERR(41) DEFINE_ISR_NOERR(42) DEFINE_ISR_NOERR(43)
DEFINE_ISR_NOERR(44) DEFINE_ISR_NOERR(45) DEFINE_ISR_NOERR(46) DEFINE_ISR_NOERR(47)

// Syscall / user
DEFINE_ISR_NOERR(128)

__attribute__((naked)) void isr_common_stub(void) {
    asm volatile(
        "push %rax\n"
        "push %rbx\n"
        "push %rcx\n"
        "push %rdx\n"
        "push %rsi\n"
        "push %rdi\n"
        "push %rbp\n"
        "push %r8\n"
        "push %r9\n"
        "push %r10\n"
        "push %r11\n"
        "push %r12\n"
        "push %r13\n"
        "push %r14\n"
        "push %r15\n"
        "mov %rsp, %rdi\n"
        "call dispatch_interrupt\n"
        "add $120, %rsp\n"
        "add $16, %rsp\n"
        "iretq\n"
    );
}

static void idt_set_gate(uint8_t num, uint64_t handler, uint8_t type_attr) {
    idt[num].offset_low = handler & 0xFFFF;
    idt[num].selector = 0x08;
    idt[num].ist = 0;
    idt[num].type_attr = type_attr;
    idt[num].offset_mid = (handler >> 16) & 0xFFFF;
    idt[num].offset_high = (handler >> 32) & 0xFFFFFFFF;
    idt[num].zero = 0;
}

static void idt_install(void) {
    memset(idt, 0, sizeof(idt));

#define INSTALL_ISR(num, attr) idt_set_gate((uint8_t)(num), (uint64_t)isr_##num, (attr))

    INSTALL_ISR(0, 0x8E);   INSTALL_ISR(1, 0x8E);   INSTALL_ISR(2, 0x8E);   INSTALL_ISR(3, 0x8E);
    INSTALL_ISR(4, 0x8E);   INSTALL_ISR(5, 0x8E);   INSTALL_ISR(6, 0x8E);   INSTALL_ISR(7, 0x8E);
    INSTALL_ISR(8, 0x8E);   INSTALL_ISR(9, 0x8E);   INSTALL_ISR(10, 0x8E);  INSTALL_ISR(11, 0x8E);
    INSTALL_ISR(12, 0x8E);  INSTALL_ISR(13, 0x8E);  INSTALL_ISR(14, 0x8E);  INSTALL_ISR(15, 0x8E);
    INSTALL_ISR(16, 0x8E);  INSTALL_ISR(17, 0x8E);  INSTALL_ISR(18, 0x8E);  INSTALL_ISR(19, 0x8E);
    INSTALL_ISR(20, 0x8E);  INSTALL_ISR(21, 0x8E);  INSTALL_ISR(22, 0x8E);  INSTALL_ISR(23, 0x8E);
    INSTALL_ISR(24, 0x8E);  INSTALL_ISR(25, 0x8E);  INSTALL_ISR(26, 0x8E);  INSTALL_ISR(27, 0x8E);
    INSTALL_ISR(28, 0x8E);  INSTALL_ISR(29, 0x8E);  INSTALL_ISR(30, 0x8E);  INSTALL_ISR(31, 0x8E);

    INSTALL_ISR(32, 0x8E);  INSTALL_ISR(33, 0x8E);  INSTALL_ISR(34, 0x8E);  INSTALL_ISR(35, 0x8E);
    INSTALL_ISR(36, 0x8E);  INSTALL_ISR(37, 0x8E);  INSTALL_ISR(38, 0x8E);  INSTALL_ISR(39, 0x8E);
    INSTALL_ISR(40, 0x8E);  INSTALL_ISR(41, 0x8E);  INSTALL_ISR(42, 0x8E);  INSTALL_ISR(43, 0x8E);
    INSTALL_ISR(44, 0x8E);  INSTALL_ISR(45, 0x8E);  INSTALL_ISR(46, 0x8E);  INSTALL_ISR(47, 0x8E);

    INSTALL_ISR(128, 0xEE);

    idtr.limit = sizeof(idt) - 1;
    idtr.base = (uint64_t)&idt;
    asm volatile ("lidt %0" : : "m"(idtr));
}

void interrupts_register_handler(uint8_t vector, interrupt_handler_t handler) {
    interrupt_handlers[vector] = handler;
}

void interrupts_init(void) {
    pic_remap();
    memset(interrupt_handlers, 0, sizeof(interrupt_handlers));

    interrupts_register_handler(32, timer_handler);
    idt_install();
    try_enable_apic();

    log_ok("interrupts", "IDT loaded and base interrupt handlers registered");
}

void interrupts_enable(void) {
    asm volatile ("sti" ::: "memory");
}

void interrupts_disable(void) {
    asm volatile ("cli" ::: "memory");
}

bool interrupts_apic_enabled(void) {
    return lapic_enabled;
}
