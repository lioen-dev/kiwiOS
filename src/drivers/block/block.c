#include "drivers/block/block.h"
#include "drivers/ahci/ahci.h"
#include "core/log.h"
#include "memory/pmm.h"
#include "memory/hhdm.h"
#include "libc/string.h"

#define MAX_PARTITIONS 16

static block_device_t g_boot = {0};
static bool g_ready = false;

static part_table_type_t g_part_table = PART_TABLE_NONE;

typedef struct {
    block_device_t* parent;
    uint64_t lba_start;
    uint64_t lba_count;

    // For MBR:
    uint8_t  mbr_type;

    // For GPT:
    bool     is_gpt;
    uint8_t  gpt_type_guid[16];
    uint8_t  gpt_part_guid[16];

    char     name_buf[24];
} part_ctx_t;

static block_device_t g_parts[MAX_PARTITIONS];
static part_ctx_t     g_part_ctx[MAX_PARTITIONS];
static uint32_t       g_part_count = 0;

// ---------------- boot device ops ----------------

static bool boot_read(block_device_t* dev, uint64_t lba, uint32_t count, void* buffer) {
    (void)dev;
    return ahci_read(lba, count, buffer);
}

static bool boot_write(block_device_t* dev, uint64_t lba, uint32_t count, const void* buffer) {
    (void)dev;
    return ahci_write(lba, count, buffer);
}

static bool boot_flush(block_device_t* dev) {
    (void)dev;
    return ahci_flush();
}

// ---------------- partition device ops ----------------

static bool part_read(block_device_t* dev, uint64_t lba, uint32_t count, void* buffer) {
    if (!dev || !dev->ctx) return false;
    part_ctx_t* p = (part_ctx_t*)dev->ctx;

    if (!p->parent || !p->parent->read) return false;
    if (count == 0) return false;

    if (p->lba_count != 0) {
        if (lba >= p->lba_count) return false;
        if ((uint64_t)count > (p->lba_count - lba)) return false;
    }

    return p->parent->read(p->parent, p->lba_start + lba, count, buffer);
}

static bool part_write(block_device_t* dev, uint64_t lba, uint32_t count, const void* buffer) {
    if (!dev || !dev->ctx) return false;
    part_ctx_t* p = (part_ctx_t*)dev->ctx;

    if (!p->parent || !p->parent->write) return false;
    if (count == 0) return false;

    if (p->lba_count != 0) {
        if (lba >= p->lba_count) return false;
        if ((uint64_t)count > (p->lba_count - lba)) return false;
    }

    return p->parent->write(p->parent, p->lba_start + lba, count, buffer);
}

static bool part_flush(block_device_t* dev) {
    if (!dev || !dev->ctx) return false;
    part_ctx_t* p = (part_ctx_t*)dev->ctx;
    if (!p->parent || !p->parent->flush) return true; // treat missing flush as OK/no-op
    return p->parent->flush(p->parent);
}

// ---------------- helpers ----------------

static bool guid_is_zero(const uint8_t g[16]) {
    for (int i = 0; i < 16; i++) if (g[i] != 0) return false;
    return true;
}

static void make_part_name(part_ctx_t* ctx, const char* parent_name, uint32_t part_number_one_based) {
    // Name like "ahci0p1" / "ahci0p12"
    memset(ctx->name_buf, 0, sizeof(ctx->name_buf));

    const char* base = parent_name ? parent_name : "disk";
    uint32_t j = 0;

    while (base[j] && j < (sizeof(ctx->name_buf) - 1)) {
        ctx->name_buf[j] = base[j];
        j++;
    }

    if (j < (sizeof(ctx->name_buf) - 1)) ctx->name_buf[j++] = 'p';

    // append decimal digits for part_number_one_based
    char tmp[4];
    uint32_t n = part_number_one_based;
    uint32_t tlen = 0;
    if (n == 0) n = 1;

    while (n > 0 && tlen < 3) {
        tmp[tlen++] = (char)('0' + (n % 10));
        n /= 10;
    }

    for (int k = (int)tlen - 1; k >= 0; k--) {
        if (j < (sizeof(ctx->name_buf) - 1)) {
            ctx->name_buf[j++] = tmp[k];
        }
    }
    ctx->name_buf[j] = 0;
}

static void register_partition(block_device_t* parent,
                               uint64_t start, uint64_t count,
                               bool is_gpt,
                               uint8_t mbr_type,
                               const uint8_t gpt_type_guid[16],
                               const uint8_t gpt_part_guid[16]) {
    if (g_part_count >= MAX_PARTITIONS) return;
    if (count == 0) return;

    uint32_t idx = g_part_count++;

    part_ctx_t* c = &g_part_ctx[idx];
    block_device_t* d = &g_parts[idx];

    memset(c, 0, sizeof(*c));
    memset(d, 0, sizeof(*d));

    c->parent = parent;
    c->lba_start = start;
    c->lba_count = count;
    c->mbr_type = mbr_type;
    c->is_gpt = is_gpt;

    if (is_gpt) {
        if (gpt_type_guid) memcpy(c->gpt_type_guid, gpt_type_guid, 16);
        if (gpt_part_guid) memcpy(c->gpt_part_guid, gpt_part_guid, 16);
    }

    make_part_name(c, parent->name, idx + 1);

    d->name = c->name_buf;
    d->sector_size = parent->sector_size;
    d->total_sectors = count;
    d->ctx = c;
    d->read = part_read;
    d->write = part_write;
    d->flush = part_flush;
}

// ---------------- MBR parsing ----------------

typedef struct __attribute__((packed)) {
    uint8_t  status;      // 0x80 bootable
    uint8_t  chs_first[3];
    uint8_t  type;
    uint8_t  chs_last[3];
    uint32_t lba_start;
    uint32_t lba_count;
} mbr_part_entry_t;

static uint32_t probe_mbr_partitions(block_device_t* parent, const uint8_t* lba0_512) {
    const uint8_t* mbr = lba0_512;

    if (!(mbr[510] == 0x55 && mbr[511] == 0xAA)) {
        log_info("block", "No valid MBR signature; skipping MBR partitions");
        return 0;
    }

    const mbr_part_entry_t* pe = (const mbr_part_entry_t*)(mbr + 446);

    uint32_t added = 0;
    for (uint32_t i = 0; i < 4 && g_part_count < MAX_PARTITIONS; i++) {
        uint8_t type = pe[i].type;
        uint32_t start = pe[i].lba_start;
        uint32_t count = pe[i].lba_count;

        if (type == 0 || count == 0) continue;

        // If this is a protective MBR (type 0xEE), that's for GPT; don't treat it as real MBR partitions.
        // We'll only see this when GPT probe failed; still, it's better not to mislead.
        if (type == 0xEE) {
            log_info("block", "Protective MBR (0xEE) detected; not registering as MBR partitions");
            continue;
        }

        register_partition(parent,
                           (uint64_t)start,
                           (uint64_t)count,
                           false,
                           type,
                           NULL,
                           NULL);

        log_okf("block", "MBR partition %u: type=%x start=%u count=%u name=%s",
                (unsigned)(g_part_count - 1),
                (unsigned)type,
                start,
                count,
                g_parts[g_part_count - 1].name);

        added++;
    }

    return added;
}

// ---------------- GPT parsing ----------------

typedef struct __attribute__((packed)) {
    uint8_t  signature[8];      // "EFI PART"
    uint32_t revision;
    uint32_t header_size;
    uint32_t header_crc32;
    uint32_t reserved;
    uint64_t current_lba;
    uint64_t backup_lba;
    uint64_t first_usable_lba;
    uint64_t last_usable_lba;
    uint8_t  disk_guid[16];
    uint64_t part_entry_lba;
    uint32_t num_part_entries;
    uint32_t part_entry_size;
    uint32_t part_array_crc32;
} gpt_header_t;

typedef struct __attribute__((packed)) {
    uint8_t  type_guid[16];
    uint8_t  unique_guid[16];
    uint64_t first_lba;
    uint64_t last_lba;
    uint64_t attrs;
    uint16_t name_utf16le[36]; // 72 bytes
} gpt_entry_t;

static bool probe_gpt_partitions(block_device_t* parent) {
    if (!parent || !parent->read) return false;
    if (parent->sector_size != 512) return false;

    // Read GPT header at LBA1
    void* phys = pmm_alloc();
    if (!phys) {
        log_error("block", "GPT probe: pmm_alloc failed");
        return false;
    }
    uint8_t* hdrb = (uint8_t*)hhdm_phys_to_virt((uint64_t)(uintptr_t)phys);
    memset(hdrb, 0, 512);

    if (!parent->read(parent, 1, 1, hdrb)) {
        log_error("block", "GPT probe: failed to read LBA1");
        return false;
    }

    const gpt_header_t* gh = (const gpt_header_t*)hdrb;

    static const uint8_t sig[8] = {'E','F','I',' ','P','A','R','T'};
    for (int i = 0; i < 8; i++) {
        if (gh->signature[i] != sig[i]) {
            return false; // no GPT
        }
    }

    if (gh->header_size < 92 || gh->header_size > 512) {
        log_errorf("block", "GPT header_size looks wrong: %u", gh->header_size);
        return false;
    }
    if (gh->part_entry_size < sizeof(gpt_entry_t) || gh->part_entry_size > 1024) {
        log_errorf("block", "GPT part_entry_size unsupported: %u", gh->part_entry_size);
        return false;
    }
    if (gh->num_part_entries == 0 || gh->num_part_entries > 4096) {
        log_errorf("block", "GPT num_part_entries suspicious: %u", gh->num_part_entries);
        return false;
    }

    uint64_t entries_lba = gh->part_entry_lba;
    uint32_t entry_size = gh->part_entry_size;
    uint32_t nentries = gh->num_part_entries;

    uint64_t total_bytes = (uint64_t)entry_size * (uint64_t)nentries;
    uint32_t sectors_needed = (uint32_t)((total_bytes + 511) / 512);
    if (sectors_needed == 0) return false;

    // Cap: avoid huge allocation early
    if (sectors_needed > 1024) {
        log_errorf("block", "GPT entries too large to read (%u sectors)", sectors_needed);
        return false;
    }

    size_t pages = (sectors_needed * 512u + PAGE_SIZE - 1) / PAGE_SIZE;
    void* phys2 = pmm_alloc_pages(pages);
    if (!phys2) {
        log_error("block", "GPT probe: pmm_alloc_pages failed for entries");
        return false;
    }

    uint8_t* ent = (uint8_t*)hhdm_phys_to_virt((uint64_t)(uintptr_t)phys2);
    memset(ent, 0, pages * PAGE_SIZE);

    if (!parent->read(parent, entries_lba, sectors_needed, ent)) {
        log_error("block", "GPT probe: failed to read partition entries");
        pmm_free_pages(phys2, pages);
        return false;
    }

    uint32_t added = 0;
    for (uint32_t i = 0; i < nentries && g_part_count < MAX_PARTITIONS; i++) {
        const uint8_t* ebase = ent + (uint64_t)i * (uint64_t)entry_size;
        const gpt_entry_t* ge = (const gpt_entry_t*)ebase;

        if (guid_is_zero(ge->type_guid)) continue;
        if (ge->first_lba == 0 && ge->last_lba == 0) continue;
        if (ge->last_lba < ge->first_lba) continue;

        uint64_t start = ge->first_lba;
        uint64_t count = (ge->last_lba - ge->first_lba) + 1;

        register_partition(parent,
                           start,
                           count,
                           true,
                           0,
                           ge->type_guid,
                           ge->unique_guid);

        log_okf("block", "GPT partition %u: start=%x count=%x name=%s",
                (unsigned)(g_part_count - 1),
                (uint32_t)start,
                (uint32_t)count,
                g_parts[g_part_count - 1].name);

        added++;
    }

    pmm_free_pages(phys2, pages);

    if (added == 0) {
        log_info("block", "GPT present but no partitions found");
    } else {
        log_okf("block", "GPT probe: %u partitions registered", added);
    }

    return true;
}

// ---------------- public API ----------------

void block_init(void) {
    // reset state
    g_part_count = 0;
    g_part_table = PART_TABLE_NONE;
    memset(g_parts, 0, sizeof(g_parts));
    memset(g_part_ctx, 0, sizeof(g_part_ctx));

    if (!ahci_disk_ready()) {
        log_error("block", "No AHCI disk ready; boot block device not available");
        g_ready = false;
        return;
    }

    g_boot.name = "ahci0";
    g_boot.sector_size = 512;
    g_boot.total_sectors = 0; // unknown for now
    g_boot.ctx = 0;
    g_boot.read = boot_read;
    g_boot.write = boot_write;
    g_boot.flush = boot_flush;

    g_ready = true;
    log_okf("block", "Boot block device: %s (sector=%u)", g_boot.name, g_boot.sector_size);

    // Read LBA0 once (for MBR signature / fallback)
    void* phys0 = pmm_alloc();
    if (!phys0) {
        log_error("block", "Partition probe: pmm_alloc failed for LBA0");
        return;
    }
    uint8_t* lba0 = (uint8_t*)hhdm_phys_to_virt((uint64_t)(uintptr_t)phys0);
    memset(lba0, 0, 512);

    if (!g_boot.read(&g_boot, 0, 1, lba0)) {
        log_error("block", "Partition probe: failed to read LBA0");
        return;
    }

    // Prefer GPT if present.
    bool has_gpt = probe_gpt_partitions(&g_boot);
    if (has_gpt) {
        g_part_table = PART_TABLE_GPT;
        return;
    }

    uint32_t added_mbr = probe_mbr_partitions(&g_boot, lba0);
    if (added_mbr > 0) {
        g_part_table = PART_TABLE_MBR;
    } else {
        g_part_table = PART_TABLE_NONE;
    }
}

block_device_t* block_boot_device(void) {
    if (!g_ready) return 0;
    return &g_boot;
}

uint32_t block_partition_count(void) {
    return g_part_count;
}

block_device_t* block_partition_device(uint32_t index) {
    if (index >= g_part_count) return 0;
    return &g_parts[index];
}

part_table_type_t block_partition_table_type(void) {
    return g_part_table;
}
