#pragma once
#include <stdint.h>
#include <stdbool.h>

typedef struct block_device block_device_t;

typedef bool (*block_read_fn)(block_device_t* dev, uint64_t lba, uint32_t count, void* buffer);
typedef bool (*block_write_fn)(block_device_t* dev, uint64_t lba, uint32_t count, const void* buffer);
typedef bool (*block_flush_fn)(block_device_t* dev);

typedef enum {
    PART_TABLE_NONE = 0,
    PART_TABLE_MBR  = 1,
    PART_TABLE_GPT  = 2
} part_table_type_t;

struct block_device {
    const char* name;
    uint32_t sector_size;     // usually 512

    // Total size (in sectors) if known; 0 if unknown.
    uint64_t total_sectors;

    // Driver-private pointer (for partitions/wrappers).
    void* ctx;

    block_read_fn  read;
    block_write_fn write;
    block_flush_fn flush;     // may be NULL if unsupported
};

// Initialize the "boot disk" block device (currently AHCI-first-disk),
// and probe partitions (GPT first, then MBR fallback).
void block_init(void);

// Returns the selected boot block device, or NULL if none.
block_device_t* block_boot_device(void);

// Partition enumeration
uint32_t block_partition_count(void);
block_device_t* block_partition_device(uint32_t index);

// Returns which partition table was detected on the boot disk.
part_table_type_t block_partition_table_type(void);
