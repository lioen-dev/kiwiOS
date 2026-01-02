#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "core/boot.h"
#include "core/console.h"
#include "core/keyboard.h"
#include "core/log.h"
#include "drivers/block/block.h"
#include "libc/string.h"
#include "memory/heap.h"
#include "memory/pmm.h"
#include "memory/vmm.h"
#include "memory/hhdm.h"
#include "fs/bcache.h"

static void print_byte_hex(struct limine_framebuffer *fb, uint8_t b) {
    static const char* hex = "0123456789ABCDEF";
    putc_fb(fb, hex[(b >> 4) & 0xF]);
    putc_fb(fb, hex[b & 0xF]);
}

static bool parse_u64(const char* s, uint64_t* out) {
    if (!s) return false;
    while (*s == ' ') s++;
    if (*s == '\0') return false;

    uint64_t v = 0;
    bool any = false;
    while (*s >= '0' && *s <= '9') {
        any = true;
        v = v * 10 + (uint64_t)(*s - '0');
        s++;
    }
    if (!any) return false;
    *out = v;
    return true;
}

static bool parse_u32(const char* s, uint32_t* out) {
    uint64_t v = 0;
    if (!parse_u64(s, &v)) return false;
    if (v > 0xFFFFFFFFULL) return false;
    *out = (uint32_t)v;
    return true;
}

static bool parse_u8(const char* s, uint8_t* out) {
    uint64_t v = 0;
    if (!parse_u64(s, &v)) return false;
    if (v > 0xFFULL) return false;
    *out = (uint8_t)v;
    return true;
}

static const char* skip_token(const char* s) {
    if (!s) return s;
    while (*s == ' ') s++;
    while (*s && *s != ' ') s++;
    while (*s == ' ') s++;
    return s;
}

// ================= Command functions =================
static void cmd_help(struct limine_framebuffer *fb) {
    print(fb, "Available commands:\n\n");
    print(fb, "  help       - Show this help message\n");
    print(fb, "  clear      - Clear the console\n");
    print(fb, "  echo [msg] - Print a message\n");
    print(fb, "  about      - Show information about KiwiOS\n");
    print(fb, "  crash [n]  - Trigger exception number n\n");
    print(fb, "  meminfo    - Show memory usage information\n");
    print(fb, "  memtest    - Run a memory test\n");
    print(fb, "  vmtest     - Run a VMM test\n");
    print(fb, "  heaptest   - Run a heap allocation test\n");
    print(fb, "  fbinfo     - Show framebuffer details\n");
    print(fb, "  scale [factor] - Set framebuffer scaling factor\n");
    print(fb, "\n");
    print(fb, "Disk commands:\n");
    print(fb, "  rawread   <lba> [count]                 - Read boot disk sectors and hex-dump first 256 bytes\n");
    print(fb, "  rawwrite  <lba> [count] <byte>          - Write pattern to boot disk then read back + verify\n");
    print(fb, "  rawflush                                - Flush boot disk write cache\n");
    print(fb, "  partlist                                - List partitions\n");
    print(fb, "  diskreadp  <part> <lba> [count]         - Read from partition device\n");
    print(fb, "  diskwritep <part> <lba> [count] <byte>  - Write to partition device and verify\n");
    print(fb, "  diskflushp <part>                       - Flush through partition device\n");
    print(fb, "  disktest                                - Quick test: write/read/verify at LBA 2048\n");
    print(fb, "  bcachestat                              - Show block cache statistics\n");
    print(fb, "  bcacheflush                             - Flush all dirty buffers\n");
    print(fb, "  bcacheflushp <part>                     - Flush partition buffers\n");
}

static void cmd_clear(struct limine_framebuffer *fb /*unused*/) {
    (void)fb;
    console_clear();
}

static void cmd_echo(struct limine_framebuffer *fb, const char *args) {
    if (args && *args) {
        print(fb, args);
        print(fb, "\n");
    } else {
        print(fb, "\n");
    }
}

static void cmd_about(struct limine_framebuffer *fb) {
    print(fb, "KiwiOS v0.1\n");
    print(fb, "A simple operating system\n");
}

static void cmd_crash(struct limine_framebuffer *fb, const char *args) {
    int exception_num = 0; // default to divide by zero

    // Parse exception number from args
    if (args && *args) {
        exception_num = 0; // Reset to 0 when parsing
        // Simple string to int conversion
        while (*args >= '0' && *args <= '9') {
            exception_num = exception_num * 10 + (*args - '0');
            args++;
        }
    }

    print(fb, "Triggering exception ");
    print_hex(fb, exception_num);
    print(fb, "...\n");

    // Trigger the appropriate exception
    switch (exception_num) {
        case 0: { // Division by zero
            volatile int x = 1;
            volatile int y = 0;
            volatile int z = x / y;
            (void)z;
            break;
        }
        case 1: // Debug - use int instruction
            asm volatile ("int $1");
            break;
        case 2: // Non Maskable Interrupt
            asm volatile ("int $2");
            break;
        case 3: // Breakpoint
            asm volatile ("int3");
            break;
        case 4: // Overflow
            asm volatile ("int $4");
            break;
        case 5: { // Bound range exceeded
            asm volatile ("int $5");
            break;
        }
        case 6: // Invalid opcode
            asm volatile ("ud2");
            break;
        case 7: { // Device not available (FPU)
            asm volatile (
                "clts\n"
                "fninit\n"
                "mov $0, %%rax\n"
                "mov %%rax, %%cr0\n"
                "fld1\n"
                ::: "rax"
            );
            asm volatile ("int $7");
            break;
        }
        case 8:
            asm volatile ("int $8");
            break;
        case 10:
            asm volatile ("int $10");
            break;
        case 11:
            asm volatile ("int $11");
            break;
        case 12:
            asm volatile ("int $12");
            break;
        case 13: {
            asm volatile ("mov $0xFFFF, %%ax; mov %%ax, %%ds" ::: "rax");
            break;
        }
        case 14: {
            volatile uint64_t *ptr = (uint64_t *)0xFFFFFFFF80000000ULL;
            volatile uint64_t val = *ptr;
            (void)val;
            break;
        }
        case 16:
            asm volatile ("int $16");
            break;
        case 17:
            asm volatile ("int $17");
            break;
        case 18:
            asm volatile ("int $18");
            break;
        case 19:
            asm volatile ("int $19");
            break;
        case 20:
            asm volatile ("int $20");
            break;
        case 21:
            asm volatile ("int $21");
            break;
        default:
            print(fb, "Exception number not supported or reserved.\n");
            print(fb, "Supported: 0-8, 10-14, 16-21\n");
            return;
    }
}

static void cmd_meminfo(struct limine_framebuffer *fb) {
    size_t total, used, free;
    pmm_get_stats(&total, &used, &free);

    print(fb, "Memory Information:\n");
    print(fb, "  Total pages: ");
    print_hex(fb, total);
    print(fb, " (");
    print_hex(fb, total * 4); // KB
    print(fb, " KB)\n");

    print(fb, "  Used pages:  ");
    print_hex(fb, used);
    print(fb, " (");
    print_hex(fb, used * 4);
    print(fb, " KB)\n");

    print(fb, "  Free pages:  ");
    print_hex(fb, free);
    print(fb, " (");
    print_hex(fb, free * 4);
    print(fb, " KB)\n");
}

static void cmd_memtest(struct limine_framebuffer *fb) {
    print(fb, "Testing memory allocation...\n");

    // Allocate a single page
    void* page1 = pmm_alloc();
    print(fb, "Allocated page at: ");
    print_hex(fb, (uint64_t)page1);
    print(fb, "\n");

    // Allocate another page
    void* page2 = pmm_alloc();
    print(fb, "Allocated page at: ");
    print_hex(fb, (uint64_t)page2);
    print(fb, "\n");

    // Allocate 10 contiguous pages
    void* pages = pmm_alloc_pages(10);
    if (pages) {
        print(fb, "Allocated 10 pages at: ");
        print_hex(fb, (uint64_t)pages);
        print(fb, "\n");
    } else {
        print(fb, "Failed to allocate 10 pages!\n");
    }

    // Free them
    print(fb, "Freeing allocations...\n");
    pmm_free(page1);
    pmm_free(page2);
    if (pages) pmm_free_pages(pages, 10);

    print(fb, "Memory test complete!\n");
}

static void cmd_vmtest(struct limine_framebuffer *fb) {
    print(fb, "Testing Virtual Memory Manager...\n");

    // Create a new page table
    page_table_t* test_pt = vmm_create_page_table();
    if (!test_pt) {
        print(fb, "Failed to create page table!\n");
        return;
    }
    print(fb, "Created page table at: ");
    print_hex(fb, (uint64_t)test_pt);
    print(fb, "\n");

    // Allocate a physical page
    uint64_t phys_page = (uint64_t)pmm_alloc();
    if (!phys_page) {
        print(fb, "Failed to allocate physical page!\n");
        return;
    }
    print(fb, "Allocated physical page: ");
    print_hex(fb, phys_page);
    print(fb, "\n");

    // Map it to a virtual address
    uint64_t virt_addr = 0x400000;
    bool mapped = vmm_map_page(test_pt, virt_addr, phys_page, PAGE_WRITE | PAGE_USER);
    if (!mapped) {
        print(fb, "Failed to map page!\n");
        pmm_free((void*)phys_page);
        return;
    }
    print(fb, "Mapped virtual ");
    print_hex(fb, virt_addr);
    print(fb, " -> physical ");
    print_hex(fb, phys_page);
    print(fb, "\n");

    // Verify the mapping
    uint64_t phys_result = vmm_get_physical(test_pt, virt_addr);
    if (phys_result == phys_page) {
        print(fb, "Mapping verified successfully!\n");
    } else {
        print(fb, "Mapping verification FAILED!\n");
        print(fb, "Expected: ");
        print_hex(fb, phys_page);
        print(fb, "\nGot: ");
        print_hex(fb, phys_result);
        print(fb, "\n");
    }

    // Test unmapping
    vmm_unmap_page(test_pt, virt_addr);
    phys_result = vmm_get_physical(test_pt, virt_addr);
    if (phys_result == 0) {
        print(fb, "Unmapping successful!\n");
    } else {
        print(fb, "Unmapping FAILED!\n");
    }

    // Clean up
    pmm_free((void*)phys_page);

    print(fb, "VMM test complete!\n");
}

static void cmd_heaptest(struct limine_framebuffer *fb) {
    print(fb, "Testing heap allocator...\n");

    // Test 1: Simple allocation
    char* str1 = (char*)kmalloc(32);
    if (str1) {
        print(fb, "Allocated 32 bytes at: ");
        print_hex(fb, (uint64_t)str1);
        print(fb, "\n");
    }

    // Test 2: Multiple allocations
    int* numbers = (int*)kmalloc(10 * sizeof(int));
    if (numbers) {
        print(fb, "Allocated array at: ");
        print_hex(fb, (uint64_t)numbers);
        print(fb, "\n");
    }

    // Test 3: Calloc (zeroed memory)
    uint64_t* zeroed = (uint64_t*)kcalloc(5, sizeof(uint64_t));
    if (zeroed) {
        print(fb, "Allocated zeroed memory at: ");
        print_hex(fb, (uint64_t)zeroed);
        print(fb, "\n");
    }

    // Show stats
    size_t allocated, free_mem, allocs;
    heap_get_stats(&allocated, &free_mem, &allocs);
    print(fb, "Heap stats:\n");
    print(fb, "  Allocated: ");
    print_hex(fb, allocated);
    print(fb, " bytes\n");
    print(fb, "  Free: ");
    print_hex(fb, free_mem);
    print(fb, " bytes\n");
    print(fb, "  Active allocations: ");
    print_hex(fb, allocs);
    print(fb, "\n");

    // Free everything
    kfree(str1);
    kfree(numbers);
    kfree(zeroed);

    print(fb, "Freed all allocations\n");

    heap_get_stats(&allocated, &free_mem, &allocs);
    print(fb, "After free - Active allocations: ");
    print_hex(fb, allocs);
    print(fb, "\n");
}

static void cmd_fbinfo(struct limine_framebuffer *fb_unused) {
    (void)fb_unused;

    struct limine_framebuffer_response *response = boot_framebuffer_response();
    if (!response || response->framebuffer_count == 0) {
        print(NULL, "No framebuffers from Limine.\n");
        return;
    }

    uint64_t count = response->framebuffer_count;
    print(NULL, "Framebuffers: ");
    print_u64(NULL, count);
    print(NULL, "\n");

    for (uint64_t i = 0; i < count; i++) {
        struct limine_framebuffer *fb = response->framebuffers[i];
        if (!fb) continue;

        print(NULL, "FB#"); print_u64(NULL, i); print(NULL, ": ");
        print_u64(NULL, fb->width);  print(NULL, "x");
        print_u64(NULL, fb->height); print(NULL, "@");
        print_u64(NULL, fb->bpp);    print(NULL, "  pitch=");
        print_u64(NULL, fb->pitch);
        print(NULL, " bytes\n");

        print(NULL, "  mem_model=");
        print_u64(NULL, fb->memory_model);
        print(NULL, "  R(");
        print_u64(NULL, fb->red_mask_size);   print(NULL, ":");
        print_u64(NULL, fb->red_mask_shift);  print(NULL, ")  G(");
        print_u64(NULL, fb->green_mask_size); print(NULL, ":");
        print_u64(NULL, fb->green_mask_shift);print(NULL, ")  B(");
        print_u64(NULL, fb->blue_mask_size);  print(NULL, ":");
        print_u64(NULL, fb->blue_mask_shift); print(NULL, ")\n");

        print(NULL, "  edid=");
        if (fb->edid && fb->edid_size) {
            print_u64(NULL, fb->edid_size); print(NULL, " bytes\n");
        } else {
            print(NULL, "none\n");
        }

        if (fb->mode_count && fb->modes) {
            uint64_t mcount = fb->mode_count;
            print(NULL, "  modes=");
            print_u64(NULL, mcount);
            print(NULL, " (showing up to 10)\n");

            uint64_t show = mcount > 10 ? 10 : mcount;
            for (uint64_t j = 0; j < show; j++) {
                struct limine_video_mode *m = fb->modes[j];
                if (!m) continue;

                print(NULL, "    [");
                print_u64(NULL, j);
                print(NULL, "] ");
                print_u64(NULL, m->width);  print(NULL, "x");
                print_u64(NULL, m->height); print(NULL, "@");
                print_u64(NULL, m->bpp);
                print(NULL, "  pitch=");
                print_u64(NULL, m->pitch);
                print(NULL, "  mem_model=");
                print_u64(NULL, m->memory_model);
                print(NULL, "\n");
            }
        } else {
            print(NULL, "  modes=none\n");
        }

        print(NULL, "\n");
    }
}

static void cmd_scale(struct limine_framebuffer *fb, const char *args) {
    (void)fb;
    uint32_t s = 0;
    if (args) {
        while (*args == ' ') args++;
        while (*args >= '0' && *args <= '9') {
            s = s * 10 + (uint32_t)(*args - '0');
            args++;
        }
    }
    if (s == 0) s = 1;
    if (s > 16) s = 9;

    console_set_scale(s);

    print(NULL, "scale set to ");
    char buf[12]; int i = 0; uint32_t t = s; do { buf[i++] = '0' + (t % 10); t/=10; } while (t);
    while (i--) putc_fb(NULL, buf[i]);
    print(NULL, "x\n");
}

// -------- Device helpers --------

static block_device_t* must_get_bootdev(struct limine_framebuffer *fb) {
    block_device_t* dev = block_boot_device();
    if (!dev) {
        print(fb, "No boot block device (block_init failed?)\n");
        return NULL;
    }
    if (!dev->read || !dev->write) {
        print(fb, "Boot block device missing read/write\n");
        return NULL;
    }
    if (dev->sector_size == 0) {
        print(fb, "Boot block device has invalid sector size\n");
        return NULL;
    }
    return dev;
}

static block_device_t* get_part(struct limine_framebuffer *fb, uint32_t idx) {
    (void)fb;
    return block_partition_device(idx);
}

// -------- Core disk ops (shared) --------

static bool do_diskread(struct limine_framebuffer* fb, block_device_t* dev, uint64_t lba, uint32_t count) {
    const uint32_t MAX_BYTES = 512u * 1024u; // sanity cap
    uint32_t bytes = count * dev->sector_size;
    if (bytes > MAX_BYTES) {
        print(fb, "diskread: request too large (cap = 512KiB). Reduce count.\n");
        return false;
    }

    size_t pages = (bytes + PAGE_SIZE - 1) / PAGE_SIZE;
    void* phys = pmm_alloc_pages(pages);
    if (!phys) {
        print(fb, "diskread: pmm_alloc_pages failed\n");
        return false;
    }

    void* buf = hhdm_phys_to_virt((uint64_t)(uintptr_t)phys);
    memset(buf, 0, pages * PAGE_SIZE);

    bool ok = dev->read(dev, lba, count, buf);
    if (!ok) {
        print(fb, "diskread: read failed (check logs)\n");
        pmm_free_pages(phys, pages);
        return false;
    }

    print(fb, "Read OK. First 256 bytes:\n");
    uint8_t* b = (uint8_t*)buf;
    uint32_t show = bytes < 256u ? bytes : 256u;

    for (uint32_t i = 0; i < show; i++) {
        if ((i % 16) == 0) {
            print(fb, "\n");
            print_hex(fb, i);
            print(fb, ": ");
        }
        print_byte_hex(fb, b[i]);
        putc_fb(fb, ' ');
    }
    print(fb, "\n\n");

    pmm_free_pages(phys, pages);
    return true;
}

static bool do_diskwrite(struct limine_framebuffer* fb, block_device_t* dev, uint64_t lba, uint32_t count, uint8_t pattern) {
    const uint32_t MAX_BYTES = 512u * 1024u;
    uint32_t bytes = count * dev->sector_size;
    if (bytes > MAX_BYTES) {
        print(fb, "diskwrite: request too large (cap = 512KiB). Reduce count.\n");
        return false;
    }

    size_t pages = (bytes + PAGE_SIZE - 1) / PAGE_SIZE;

    void* phys_w = pmm_alloc_pages(pages);
    void* phys_r = pmm_alloc_pages(pages);
    if (!phys_w || !phys_r) {
        print(fb, "diskwrite: pmm_alloc_pages failed\n");
        if (phys_w) pmm_free_pages(phys_w, pages);
        if (phys_r) pmm_free_pages(phys_r, pages);
        return false;
    }

    void* buf_w = phys_to_virt((uint64_t)(uintptr_t)phys_w);
    void* buf_r = phys_to_virt((uint64_t)(uintptr_t)phys_r);

    memset(buf_w, pattern, pages * PAGE_SIZE);
    memset(buf_r, 0,       pages * PAGE_SIZE);

    print(fb, "Writing...\n");
    if (!dev->write(dev, lba, count, buf_w)) {
        print(fb, "diskwrite: write failed (check logs)\n");
        pmm_free_pages(phys_w, pages);
        pmm_free_pages(phys_r, pages);
        return false;
    }

    print(fb, "Reading back...\n");
    if (!dev->read(dev, lba, count, buf_r)) {
        print(fb, "diskwrite: read-back failed (check logs)\n");
        pmm_free_pages(phys_w, pages);
        pmm_free_pages(phys_r, pages);
        return false;
    }

    uint8_t* w = (uint8_t*)buf_w;
    uint8_t* r = (uint8_t*)buf_r;

    for (uint32_t i = 0; i < bytes; i++) {
        if (w[i] != r[i]) {
            print(fb, "VERIFY FAILED at byte ");
            print_hex(fb, i);
            print(fb, ": wrote ");
            print_byte_hex(fb, w[i]);
            print(fb, " read ");
            print_byte_hex(fb, r[i]);
            print(fb, "\n");
            pmm_free_pages(phys_w, pages);
            pmm_free_pages(phys_r, pages);
            return false;
        }
    }

    print(fb, "VERIFY OK\n");

    pmm_free_pages(phys_w, pages);
    pmm_free_pages(phys_r, pages);
    return true;
}

// -------- Commands --------

static void cmd_rawread(struct limine_framebuffer *fb, const char *args) {
    print(fb, "WARNING, reading from RAW DISK, not a partition!\n");
    print(fb, "Press ENTER to continue...\n");
    char c = keyboard_getchar();
    if (c != '\n') {
        print(fb, "\nRead aborted by user.\n");
        return;
    }
    block_device_t* dev = must_get_bootdev(fb);
    if (!dev) return;

    uint64_t lba = 0;
    uint32_t count = 1;

    if (!parse_u64(args, &lba)) {
        print(fb, "Usage: rawread <lba> [count]\n");
        return;
    }

    const char* a2 = skip_token(args);
    if (a2 && *a2) {
        (void)parse_u32(a2, &count);
        if (count == 0) count = 1;
    }

    (void)do_diskread(fb, dev, lba, count);
}

static void cmd_rawwrite(struct limine_framebuffer *fb, const char *args) {
    print(fb, "WARNING, writing to RAW DISK, not a partition!\n");
    print(fb, "Press ENTER to continue...\n");
    char c = keyboard_getchar();
    if (c != '\n') {
        print(fb, "\nWrite aborted by user.\n");
        return;
    }
    block_device_t* dev = must_get_bootdev(fb);
    if (!dev) return;

    uint64_t lba = 0;
    uint32_t count = 1;
    uint8_t pattern = 0xAA;

    if (!parse_u64(args, &lba)) {
        print(fb, "Usage: rawwrite <lba> [count] <byte>\n");
        return;
    }

    const char* a2 = skip_token(args);
    const char* a3 = skip_token(a2);

    if (a2 && *a2) {
        (void)parse_u32(a2, &count);
        if (count == 0) count = 1;
    }

    if (a3 && *a3) {
        (void)parse_u8(a3, &pattern);
    } else {
        // convenience: diskwrite <lba> <byte>
        uint8_t maybe_pat = 0;
        if (a2 && *a2 && parse_u8(a2, &maybe_pat)) {
            pattern = maybe_pat;
            count = 1;
        }
    }

    (void)do_diskwrite(fb, dev, lba, count, pattern);
}

static void cmd_rawflush(struct limine_framebuffer *fb) {
    print(fb, "WARNING, writing to RAW DISK, not a partition!\n");
    print(fb, "Press ENTER to continue...\n");
    char c = keyboard_getchar();
    if (c != '\n') {
        print(fb, "\nFlush aborted by user.\n");
        return;
    }
    block_device_t* dev = must_get_bootdev(fb);
    if (!dev) return;

    if (!dev->flush) {
        print(fb, "rawflush: device does not support flush\n");
        return;
    }

    print(fb, "Flushing drive cache...\n");
    if (!dev->flush(dev)) {
        print(fb, "rawflush: flush failed (check logs)\n");
        return;
    }
    print(fb, "rawflush: OK\n");
}

static void cmd_partlist(struct limine_framebuffer* fb) {
    part_table_type_t t = block_partition_table_type();

    print(fb, "Partition table: ");
    switch (t) {
        case PART_TABLE_GPT: print(fb, "GPT\n"); break;
        case PART_TABLE_MBR: print(fb, "MBR\n"); break;
        default:             print(fb, "none\n"); break;
    }

    uint32_t n = block_partition_count();
    print(fb, "Partitions found: ");
    print_hex(fb, n);
    print(fb, "\n");

    for (uint32_t i = 0; i < n; i++) {
        block_device_t* p = block_partition_device(i);
        if (!p) continue;

        // We can also show start LBA if you want later, but we keep this minimal.
        print(fb, "  [");
        print_hex(fb, i);
        print(fb, "] ");
        print(fb, p->name ? p->name : "(noname)");
        print(fb, "  sectors=");
        print_hex(fb, p->total_sectors);
        print(fb, "\n");
    }
}

static void cmd_diskreadp(struct limine_framebuffer* fb, const char* args) {
    uint32_t idx = 0;
    uint64_t lba = 0;
    uint32_t count = 1;

    if (!parse_u32(args, &idx)) {
        print(fb, "Usage: diskreadp <part> <lba> [count]\n");
        return;
    }
    const char* a2 = skip_token(args);
    if (!parse_u64(a2, &lba)) {
        print(fb, "Usage: diskreadp <part> <lba> [count]\n");
        return;
    }
    const char* a3 = skip_token(a2);
    if (a3 && *a3) {
        (void)parse_u32(a3, &count);
        if (count == 0) count = 1;
    }

    block_device_t* dev = get_part(fb, idx);
    if (!dev) {
        print(fb, "diskreadp: invalid partition index\n");
        return;
    }
    (void)do_diskread(fb, dev, lba, count);
}

static void cmd_diskwritep(struct limine_framebuffer* fb, const char* args) {
    uint32_t idx = 0;
    uint64_t lba = 0;
    uint32_t count = 1;
    uint8_t pattern = 0xAA;

    if (!parse_u32(args, &idx)) {
        print(fb, "Usage: diskwritep <part> <lba> [count] <byte>\n");
        return;
    }
    const char* a2 = skip_token(args);
    if (!parse_u64(a2, &lba)) {
        print(fb, "Usage: diskwritep <part> <lba> [count] <byte>\n");
        return;
    }
    const char* a3 = skip_token(a2);
    const char* a4 = skip_token(a3);

    if (a3 && *a3) {
        (void)parse_u32(a3, &count);
        if (count == 0) count = 1;
    }
    if (a4 && *a4) {
        (void)parse_u8(a4, &pattern);
    } else {
        // allow: diskwritep <part> <lba> <byte>  (count=1)
        uint8_t maybe_pat = 0;
        if (a3 && *a3 && parse_u8(a3, &maybe_pat)) {
            pattern = maybe_pat;
            count = 1;
        } else {
            print(fb, "Usage: diskwritep <part> <lba> [count] <byte>\n");
            return;
        }
    }

    block_device_t* dev = get_part(fb, idx);
    if (!dev) {
        print(fb, "diskwritep: invalid partition index\n");
        return;
    }
    (void)do_diskwrite(fb, dev, lba, count, pattern);
}

static void cmd_diskflushp(struct limine_framebuffer* fb, const char* args) {
    uint32_t idx = 0;
    if (!parse_u32(args, &idx)) {
        print(fb, "Usage: diskflushp <part>\n");
        return;
    }
    block_device_t* dev = get_part(fb, idx);
    if (!dev) {
        print(fb, "diskflushp: invalid partition index\n");
        return;
    }
    if (!dev->flush) {
        print(fb, "diskflushp: device does not support flush\n");
        return;
    }
    print(fb, "Flushing...\n");
    if (!dev->flush(dev)) {
        print(fb, "diskflushp: flush failed\n");
        return;
    }
    print(fb, "diskflushp: OK\n");
}

static void cmd_disktest(struct limine_framebuffer *fb) {
    print(fb, "disktest: writing pattern 0x5A to LBA 2048, 1 sector\n");
    cmd_rawwrite(fb, "2048 1 90");
}

// -------- block cache --------

static void cmd_bcachestat(struct limine_framebuffer* fb) {
    bcache_stats_t s = bcache_stats();

    print(fb, "bcache:\n");
    print(fb, "  bufs:   used=");
    print_hex(fb, s.used_bufs);
    print(fb, " total=");
    print_hex(fb, s.total_bufs);
    print(fb, " dirty=");
    print_hex(fb, s.dirty_bufs);
    print(fb, "\n");

    print(fb, "  hits=");
    print_hex(fb, s.hits);
    print(fb, " misses=");
    print_hex(fb, s.misses);
    print(fb, " evictions=");
    print_hex(fb, s.evictions);
    print(fb, "\n");

    print(fb, "  writebacks=");
    print_hex(fb, s.writebacks);
    print(fb, " sync_calls=");
    print_hex(fb, s.sync_calls);
    print(fb, "\n");
}

static void cmd_bcacheflush(struct limine_framebuffer* fb) {
    print(fb, "bcacheflush: syncing all dirty buffers...\n");
    if (!bcache_sync_all()) {
        print(fb, "bcacheflush: FAILED (see logs)\n");
        return;
    }
    print(fb, "bcacheflush: OK\n");
}

static void cmd_bcacheflushp(struct limine_framebuffer* fb, const char* args) {
    uint32_t idx = 0;
    if (!parse_u32(args, &idx)) {
        print(fb, "Usage: bcacheflushp <part>\n");
        return;
    }

    block_device_t* dev = block_partition_device(idx);
    if (!dev) {
        print(fb, "bcacheflushp: invalid partition index\n");
        return;
    }

    print(fb, "bcacheflushp: syncing partition buffers...\n");
    if (!bcache_sync_dev(dev)) {
        print(fb, "bcacheflushp: FAILED (see logs)\n");
        return;
    }
    print(fb, "bcacheflushp: OK\n");
}

// -------- Unknown --------

static void cmd_unknown(struct limine_framebuffer *fb, const char *cmd) {
    print(fb, "Unknown command: ");
    print(fb, cmd);
    print(fb, "\n");
    print(fb, "Type 'help' for available commands\n");
}

// ================= Command dispatch =================
typedef void (*cmd_func_t)(struct limine_framebuffer *fb);

typedef enum {
    COMMAND_NO_ARGS,
    COMMAND_TAKES_ARGS
} command_arity;

struct command {
    const char *name;
    cmd_func_t func;
    command_arity arity;
};

static void execute_command(struct limine_framebuffer *fb, char *input) {
    // Skip leading spaces
    while (*input == ' ') input++;

    // Empty command
    if (*input == '\0') return;

    // Find end of command word
    char *args = input;
    while (*args && *args != ' ') args++;

    // Split command and args
    if (*args) {
        *args = '\0'; // null terminate command
        args++; // point to arguments
        while (*args == ' ') args++; // skip spaces
    }

    if (strcmp(input, "help") == 0) {
        cmd_help(fb);
        return;
    }

    if (strcmp(input, "clear") == 0) {
        cmd_clear(fb);
        return;
    }

    if (strcmp(input, "echo") == 0) {
        cmd_echo(fb, args);
        return;
    }

    if (strcmp(input, "about") == 0) {
        cmd_about(fb);
        return;
    }

    if (strcmp(input, "crash") == 0) {
        cmd_crash(fb, args);
        return;
    }

    if (strcmp(input, "meminfo") == 0) {
        cmd_meminfo(fb);
        return;
    }

    if (strcmp(input, "memtest") == 0) {
        cmd_memtest(fb);
        return;
    }

    if (strcmp(input, "vmtest") == 0) {
        cmd_vmtest(fb);
        return;
    }

    if (strcmp(input, "heaptest") == 0) {
        cmd_heaptest(fb);
        return;
    }

    if (strcmp(input, "fbinfo") == 0) {
        cmd_fbinfo(fb);
        return;
    }

    if (strcmp(input, "scale") == 0) {
        cmd_scale(fb, args);
        return;
    }

    if (strcmp(input, "rawread") == 0) {
        cmd_rawread(fb, args);
        return;
    }

    if (strcmp(input, "rawwrite") == 0) {
        cmd_rawwrite(fb, args);
        return;
    }

    if (strcmp(input, "disktest") == 0) {
        cmd_disktest(fb);
        return;
    }

    if (strcmp(input, "rawflush") == 0) {
        cmd_rawflush(fb);
        return;
    }

    if (strcmp(input, "partlist") == 0) {
        cmd_partlist(fb);
        return;
    }

    if (strcmp(input, "diskreadp") == 0) {
        cmd_diskreadp(fb, args);
        return;
    }

    if (strcmp(input, "diskwritep") == 0) {
        cmd_diskwritep(fb, args);
        return;
    }

    if (strcmp(input, "diskflushp") == 0) {
        cmd_diskflushp(fb, args);
        return;
    }

    if (strcmp(input, "bcachestat") == 0) {
        cmd_bcachestat(fb);
        return;
    }

    if (strcmp(input, "bcacheflush") == 0) {
        cmd_bcacheflush(fb);
        return;
    }

    if (strcmp(input, "bcacheflushp") == 0) {
        cmd_bcacheflushp(fb, args);
        return;
    }

    // Command not found
    cmd_unknown(fb, input);
}

// ================= Input handling =================
#define INPUT_BUFFER_SIZE 256

#define HISTORY_SIZE 32
static char history[HISTORY_SIZE][INPUT_BUFFER_SIZE];
static int history_count = 0;   // number of stored entries
static int history_cursor = -1; // -1 = live typing, 0 = newest history, ...
static char history_scratch[INPUT_BUFFER_SIZE];
static int history_scratch_len = 0;

static void history_record(const char *line) {
    if (!line || !*line) return;

    // Avoid duplicate consecutive entries
    if (history_count > 0) {
        const char *last = history[(history_count - 1) % HISTORY_SIZE];
        if (strncmp(last, line, INPUT_BUFFER_SIZE) == 0) return;
    }

    size_t len = strlen(line);
    if (len >= INPUT_BUFFER_SIZE) len = INPUT_BUFFER_SIZE - 1;

    int slot = history_count % HISTORY_SIZE;
    memcpy(history[slot], line, len);
    history[slot][len] = '\0';
    history_count++;
}

static void reset_history_navigation(void) {
    history_cursor = -1;
    history_scratch_len = 0;
}

static const char *history_fetch(int cursor_from_newest) {
    if (cursor_from_newest < 0) return NULL;
    if (cursor_from_newest >= history_count) return NULL;
    int logical = history_count - 1 - cursor_from_newest;
    return history[logical % HISTORY_SIZE];
}

static void replace_input_line(struct limine_framebuffer *fb,
                               char *buffer, int *pos,
                               const char *text) {
    while (*pos > 0) {
        putc_fb(fb, '\b');
        (*pos)--;
    }

    const char *p = text;
    while (*p && *pos < INPUT_BUFFER_SIZE - 1) {
        buffer[*pos] = *p;
        putc_fb(fb, *p);
        (*pos)++;
        p++;
    }
}

void shell_loop(struct limine_framebuffer *fb) {
    char input_buffer[INPUT_BUFFER_SIZE];
    int input_pos = 0;
    log_info("shell", "interactive shell started");

    print(fb, "Welcome to kiwiOS!\n");
    print(fb, "Type 'help' for available commands\n\n");
    print(fb, "> ");

    while (1) {
        char c = keyboard_getchar();
        if (c == KEY_ARROW_UP) {
            if (history_cursor == -1) {
                history_scratch_len = input_pos;
                if (history_scratch_len > INPUT_BUFFER_SIZE - 1) history_scratch_len = INPUT_BUFFER_SIZE - 1;
                memcpy(history_scratch, input_buffer, (size_t)history_scratch_len);
                history_scratch[history_scratch_len] = '\0';
            }

            if (history_cursor + 1 < history_count) {
                history_cursor++;
                const char *entry = history_fetch(history_cursor);
                if (entry) replace_input_line(fb, input_buffer, &input_pos, entry);
            }
            continue;
        }

        if (c == KEY_ARROW_DOWN) {
            if (history_cursor > 0) {
                history_cursor--;
                const char *entry = history_fetch(history_cursor);
                if (entry) replace_input_line(fb, input_buffer, &input_pos, entry);
            } else if (history_cursor == 0) {
                history_cursor = -1;
                replace_input_line(fb, input_buffer, &input_pos, history_scratch);
            }
            continue;
        }

        if (c == '\n') {
            // Execute command
            print(fb, "\n");
            input_buffer[input_pos] = '\0';

            if (input_pos > 0) {
                history_record(input_buffer);
                execute_command(fb, input_buffer);
            }

            // Reset for next command
            input_pos = 0;
            print(fb, "> ");
            reset_history_navigation();
        } else if (c == '\b') {
            // Backspace
            if (input_pos > 0) {
                input_pos--;
                putc_fb(fb, '\b');
            }
        } else if (input_pos < INPUT_BUFFER_SIZE - 1) {
            // Add character to buffer
            input_buffer[input_pos++] = c;
            putc_fb(fb, c);
        }
    }
}
