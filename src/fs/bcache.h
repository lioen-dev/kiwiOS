#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "drivers/block/block.h"

#define BCACHE_BLOCK_SIZE   4096u
#define BCACHE_SECTOR_SIZE  512u
#define BCACHE_SECTORS_PER_BLOCK (BCACHE_BLOCK_SIZE / BCACHE_SECTOR_SIZE)

typedef struct bcache_buf bcache_buf_t;

typedef struct {
    uint64_t hits;
    uint64_t misses;
    uint64_t evictions;
    uint64_t writebacks;
    uint64_t sync_calls;
    uint32_t total_bufs;
    uint32_t used_bufs;
    uint32_t dirty_bufs;
} bcache_stats_t;

// Initialize buffer cache with N buffers (each buffer holds one 4 KiB block).
// Recommended: 128..1024 depending on RAM. (256 => 1 MiB of cached data)
void bcache_init(uint32_t num_bufs);

// Get a cached 4 KiB block for (dev, block_no).
// The returned buffer is "pinned" (refcount++) and must be released with bcache_put().
// Returns NULL on failure.
bcache_buf_t* bcache_get(block_device_t* dev, uint64_t block_no);

// Release a pinned buffer.
void bcache_put(bcache_buf_t* b);

// Mark a buffer dirty (caller modified data and wants it written back eventually).
void bcache_mark_dirty(bcache_buf_t* b);

// Write back all dirty buffers belonging to dev. Calls dev->flush(dev) at end if available.
bool bcache_sync_dev(block_device_t* dev);

// Write back all dirty buffers for all devices. Calls each dev flush where possible.
bool bcache_sync_all(void);

// Access buffer data and metadata.
void* bcache_data(bcache_buf_t* b);
block_device_t* bcache_dev(bcache_buf_t* b);
uint64_t bcache_blockno(bcache_buf_t* b);
bool bcache_is_dirty(bcache_buf_t* b);

// Stats
bcache_stats_t bcache_stats(void);
