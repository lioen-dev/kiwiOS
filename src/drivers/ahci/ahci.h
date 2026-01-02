#pragma once
#include <stdint.h>
#include <stdbool.h>

void ahci_probe_mmio(uint32_t mmio_phys);
bool ahci_disk_ready(void);

// Read/write 512-byte sectors from the first detected SATA disk.
bool ahci_read(uint64_t lba, uint32_t sector_count, void* buffer);
bool ahci_write(uint64_t lba, uint32_t sector_count, const void* buffer);

// Force drive write cache to be committed (needed for journaling correctness).
bool ahci_flush(void);
