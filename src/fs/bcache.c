#include "fs/bcache.h"
#include "core/log.h"
#include "memory/pmm.h"
#include "memory/hhdm.h"
#include "memory/heap.h"
#include "libc/string.h"

typedef struct bcache_buf {
    // Key
    block_device_t* dev;
    uint64_t block_no;

    // State
    uint32_t refcnt;
    bool valid;
    bool dirty;

    // Data (one 4 KiB page)
    uint64_t data_phys;
    uint8_t* data_virt;

    // Hash chain
    struct bcache_buf* hnext;

    // LRU list
    struct bcache_buf* prev;
    struct bcache_buf* next;
} bcache_buf_t;

static bcache_buf_t* g_bufs = NULL;
static uint32_t g_nbufs = 0;

static bcache_buf_t** g_ht = NULL;
static uint32_t g_ht_cap = 0;

// LRU: head = most-recent, tail = least-recent
static bcache_buf_t* g_lru_head = NULL;
static bcache_buf_t* g_lru_tail = NULL;

static bcache_stats_t g_stats = {0};

// ----------------- internal helpers -----------------

static uint64_t key_hash(block_device_t* dev, uint64_t block_no) {
    // cheap pointer+block mix
    uint64_t x = (uint64_t)(uintptr_t)dev;
    x ^= block_no * 11400714819323198485ull;
    x ^= (x >> 33);
    x *= 0xff51afd7ed558ccdull;
    x ^= (x >> 33);
    return x;
}

static void lru_remove(bcache_buf_t* b) {
    if (!b) return;
    if (b->prev) b->prev->next = b->next;
    if (b->next) b->next->prev = b->prev;
    if (g_lru_head == b) g_lru_head = b->next;
    if (g_lru_tail == b) g_lru_tail = b->prev;
    b->prev = b->next = NULL;
}

static void lru_push_front(bcache_buf_t* b) {
    if (!b) return;
    b->prev = NULL;
    b->next = g_lru_head;
    if (g_lru_head) g_lru_head->prev = b;
    g_lru_head = b;
    if (!g_lru_tail) g_lru_tail = b;
}

static void lru_touch(bcache_buf_t* b) {
    if (!b) return;
    // Move to front
    lru_remove(b);
    lru_push_front(b);
}

static void ht_remove(bcache_buf_t* b) {
    if (!b || !g_ht_cap) return;
    uint32_t idx = (uint32_t)(key_hash(b->dev, b->block_no) % g_ht_cap);
    bcache_buf_t* cur = g_ht[idx];
    bcache_buf_t* prev = NULL;
    while (cur) {
        if (cur == b) {
            if (prev) prev->hnext = cur->hnext;
            else g_ht[idx] = cur->hnext;
            cur->hnext = NULL;
            return;
        }
        prev = cur;
        cur = cur->hnext;
    }
}

static void ht_insert(bcache_buf_t* b) {
    if (!b || !g_ht_cap) return;
    uint32_t idx = (uint32_t)(key_hash(b->dev, b->block_no) % g_ht_cap);
    b->hnext = g_ht[idx];
    g_ht[idx] = b;
}

static bcache_buf_t* ht_lookup(block_device_t* dev, uint64_t block_no) {
    if (!g_ht_cap) return NULL;
    uint32_t idx = (uint32_t)(key_hash(dev, block_no) % g_ht_cap);
    bcache_buf_t* cur = g_ht[idx];
    while (cur) {
        if (cur->valid && cur->dev == dev && cur->block_no == block_no) return cur;
        cur = cur->hnext;
    }
    return NULL;
}

static bool dev_read_block(block_device_t* dev, uint64_t block_no, void* out_4k) {
    if (!dev || !dev->read) return false;
    if (dev->sector_size != BCACHE_SECTOR_SIZE) {
        log_errorf("bcache", "dev_read_block: sector_size=%u unsupported", dev->sector_size);
        return false;
    }
    uint64_t lba = block_no * (uint64_t)BCACHE_SECTORS_PER_BLOCK;
    return dev->read(dev, lba, BCACHE_SECTORS_PER_BLOCK, out_4k);
}

static bool dev_write_block(block_device_t* dev, uint64_t block_no, const void* in_4k) {
    if (!dev || !dev->write) return false;
    if (dev->sector_size != BCACHE_SECTOR_SIZE) {
        log_errorf("bcache", "dev_write_block: sector_size=%u unsupported", dev->sector_size);
        return false;
    }
    uint64_t lba = block_no * (uint64_t)BCACHE_SECTORS_PER_BLOCK;
    return dev->write(dev, lba, BCACHE_SECTORS_PER_BLOCK, in_4k);
}

static bool writeback_one(bcache_buf_t* b) {
    if (!b || !b->valid) return true;
    if (!b->dirty) return true;

    if (!dev_write_block(b->dev, b->block_no, b->data_virt)) {
        log_errorf("bcache", "writeback failed dev=%s block=%x",
                   b->dev ? (b->dev->name ? b->dev->name : "(noname)") : "(null)",
                   (uint32_t)b->block_no);
        return false;
    }

    b->dirty = false;
    if (g_stats.dirty_bufs > 0) g_stats.dirty_bufs--;
    g_stats.writebacks++;
    return true;
}

static bcache_buf_t* find_evictable(void) {
    // Pick from LRU tail: least recently used, refcnt==0
    bcache_buf_t* cur = g_lru_tail;
    while (cur) {
        if (cur->refcnt == 0) return cur;
        cur = cur->prev;
    }
    return NULL;
}

// ----------------- public API -----------------

void bcache_init(uint32_t num_bufs) {
    if (num_bufs == 0) num_bufs = 128;

    g_nbufs = num_bufs;
    g_stats.total_bufs = num_bufs;

    // Hash table capacity: power-of-two-ish but simple: 2x bufs + 1
    g_ht_cap = (num_bufs * 2u) + 1u;

    g_bufs = (bcache_buf_t*)kmalloc(sizeof(bcache_buf_t) * (size_t)num_bufs);
    g_ht = (bcache_buf_t**)kmalloc(sizeof(bcache_buf_t*) * (size_t)g_ht_cap);

    if (!g_bufs || !g_ht) {
        log_error("bcache", "bcache_init: kmalloc failed");
        return;
    }

    memset(g_bufs, 0, sizeof(bcache_buf_t) * (size_t)num_bufs);
    memset(g_ht, 0, sizeof(bcache_buf_t*) * (size_t)g_ht_cap);

    // Allocate backing pages and put all bufs into LRU list.
    g_lru_head = g_lru_tail = NULL;

    for (uint32_t i = 0; i < num_bufs; i++) {
        void* phys = pmm_alloc();
        if (!phys) {
            log_errorf("bcache", "bcache_init: pmm_alloc failed at i=%u", i);
            // still continue; that buffer remains unusable
            continue;
        }

        g_bufs[i].data_phys = (uint64_t)(uintptr_t)phys;
        g_bufs[i].data_virt = (uint8_t*)hhdm_phys_to_virt(g_bufs[i].data_phys);

        g_bufs[i].valid = false;
        g_bufs[i].dirty = false;
        g_bufs[i].refcnt = 0;
        g_bufs[i].dev = NULL;
        g_bufs[i].block_no = 0;
        g_bufs[i].hnext = NULL;
        g_bufs[i].prev = g_bufs[i].next = NULL;

        lru_push_front(&g_bufs[i]);
    }

    log_okf("bcache", "Initialized %u buffers (%u KiB cached), hash=%u",
            num_bufs,
            (unsigned)((num_bufs * BCACHE_BLOCK_SIZE) / 1024u),
            g_ht_cap);
}

bcache_buf_t* bcache_get(block_device_t* dev, uint64_t block_no) {
    if (!dev) return NULL;
    if (!g_bufs || g_nbufs == 0 || !g_ht) return NULL;

    // Look up
    bcache_buf_t* b = ht_lookup(dev, block_no);
    if (b) {
        g_stats.hits++;
        b->refcnt++;
        lru_touch(b);
        return b;
    }

    g_stats.misses++;

    // Need a free buffer (evictable)
    bcache_buf_t* v = find_evictable();
    if (!v) {
        log_error("bcache", "bcache_get: no evictable buffers (all pinned)");
        return NULL;
    }

    // If valid, remove old mapping
    if (v->valid) {
        // Writeback if dirty
        if (v->dirty) {
            if (!writeback_one(v)) return NULL;
        }
        ht_remove(v);
        g_stats.evictions++;
    }

    // Fill new key
    v->dev = dev;
    v->block_no = block_no;
    v->valid = true;
    v->dirty = false;

    // Read from disk
    if (!dev_read_block(dev, block_no, v->data_virt)) {
        log_errorf("bcache", "bcache_get: read failed dev=%s block=%x",
                   dev->name ? dev->name : "(noname)",
                   (uint32_t)block_no);
        // Mark invalid so it won't poison cache
        v->valid = false;
        v->dev = NULL;
        v->block_no = 0;
        return NULL;
    }

    // Insert in hash and pin
    ht_insert(v);
    v->refcnt = 1;
    lru_touch(v);

    // Track used buffers (approx: count valid)
    // We maintain used_bufs by scanning? too expensive. Do it lazily:
    // increment when a buffer becomes valid for the first time after invalid.
    // If it was valid and got remapped, used_bufs unchanged.
    // Here: if it was invalid before fill, we should count it; but we don't know.
    // We'll maintain a simple conservative metric:
    if (g_stats.used_bufs < g_stats.total_bufs) g_stats.used_bufs++;

    return v;
}

void bcache_put(bcache_buf_t* b) {
    if (!b) return;
    if (b->refcnt == 0) return;
    b->refcnt--;
    // keep in cache; LRU already touched on get
}

void bcache_mark_dirty(bcache_buf_t* b) {
    if (!b || !b->valid) return;
    if (!b->dirty) {
        b->dirty = true;
        g_stats.dirty_bufs++;
    }
}

bool bcache_sync_dev(block_device_t* dev) {
    g_stats.sync_calls++;
    if (!dev) return false;
    if (!g_bufs) return true;

    bool ok = true;

    for (uint32_t i = 0; i < g_nbufs; i++) {
        bcache_buf_t* b = &g_bufs[i];
        if (!b->valid) continue;
        if (b->dev != dev) continue;
        if (!b->dirty) continue;
        if (!writeback_one(b)) ok = false;
    }

    if (dev->flush) {
        if (!dev->flush(dev)) ok = false;
    }

    return ok;
}

bool bcache_sync_all(void) {
    g_stats.sync_calls++;
    if (!g_bufs) return true;

    bool ok = true;

    // Writeback all dirty
    for (uint32_t i = 0; i < g_nbufs; i++) {
        bcache_buf_t* b = &g_bufs[i];
        if (!b->valid) continue;
        if (!b->dirty) continue;
        if (!writeback_one(b)) ok = false;
    }

    // Best-effort flush per device: just call flush on boot + partitions if they exist
    // (Avoid needing a global device registry here.)
    // Callers can also call bcache_sync_dev explicitly.
    return ok;
}

void* bcache_data(bcache_buf_t* b) {
    return b ? (void*)b->data_virt : NULL;
}

block_device_t* bcache_dev(bcache_buf_t* b) {
    return b ? b->dev : NULL;
}

uint64_t bcache_blockno(bcache_buf_t* b) {
    return b ? b->block_no : 0;
}

bool bcache_is_dirty(bcache_buf_t* b) {
    return b ? b->dirty : false;
}

bcache_stats_t bcache_stats(void) {
    return g_stats;
}
