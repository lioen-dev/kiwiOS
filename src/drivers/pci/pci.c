#include <stdint.h>
#include <stddef.h>
#include "arch/x86/io.h"
#include "core/log.h"
#include "drivers/ahci/ahci.h"

// PCI legacy config I/O ports
#define PCI_CONFIG_ADDR 0xCF8
#define PCI_CONFIG_DATA 0xCFC

static inline uint32_t pci_cfg_addr(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off) {
    return (uint32_t)(0x80000000u
        | ((uint32_t)bus  << 16)
        | ((uint32_t)dev  << 11)
        | ((uint32_t)func << 8)
        | (off & 0xFC));
}

static uint32_t pci_read32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off) {
    outl(PCI_CONFIG_ADDR, pci_cfg_addr(bus, dev, func, off));
    return inl(PCI_CONFIG_DATA);
}

static uint16_t pci_read16(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off) {
    uint32_t v = pci_read32(bus, dev, func, off);
    return (uint16_t)((v >> ((off & 2) * 8)) & 0xFFFF);
}

static void pci_write32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off, uint32_t value) {
    outl(PCI_CONFIG_ADDR, pci_cfg_addr(bus, dev, func, off));
    outl(PCI_CONFIG_DATA, value);
}

static void pci_write16(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off, uint16_t value) {
    // Read-modify-write the aligned 32-bit register, then write back.
    uint32_t orig = pci_read32(bus, dev, func, off);
    uint32_t shift = (uint32_t)((off & 2) * 8);
    uint32_t mask = (uint32_t)(0xFFFFu << shift);
    uint32_t next = (orig & ~mask) | ((uint32_t)value << shift);
    pci_write32(bus, dev, func, off, next);
}

static uint8_t pci_read8(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off) {
    uint32_t v = pci_read32(bus, dev, func, off);
    return (uint8_t)((v >> ((off & 3) * 8)) & 0xFF);
}

// Public: read BARn (32-bit read). BAR0..BAR5
uint32_t pci_read_bar32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t bar_index) {
    return pci_read32(bus, dev, func, (uint8_t)(0x10 + bar_index * 4));
}

static const char* pci_class_name(uint8_t class_code, uint8_t subclass, uint8_t prog_if) {
    // Minimal, just enough to recognize what you care about early
    // AHCI is class 0x01, subclass 0x06, prog_if 0x01
    if (class_code == 0x01 && subclass == 0x06 && prog_if == 0x01) return "SATA (AHCI)";
    if (class_code == 0x0C && subclass == 0x03) return "USB controller";
    if (class_code == 0x02) return "Network controller";
    if (class_code == 0x03) return "Display controller";
    return "Other";
}

void pci_enable_bus_master(uint8_t bus, uint8_t dev, uint8_t func) {
    // PCI command register at offset 0x04
    // bit 2 = Bus Master Enable
    // bit 1 = Memory Space Enable (commonly needed for MMIO BARs)
    uint16_t cmd = pci_read16(bus, dev, func, 0x04);
    uint16_t next = (uint16_t)(cmd | (1u << 2) | (1u << 1));
    if (next != cmd) {
        pci_write16(bus, dev, func, 0x04, next);
        log_infof("pci", "Enabled bus mastering: %x:%x.%u CMD %x -> %x",
                  (unsigned)bus, (unsigned)dev, (unsigned)func, cmd, next);
    }
}

void pci_enumerate_and_log(void) {
    log_info("pci", "Scanning buses 0..255");

    for (uint16_t bus = 0; bus < 256; bus++) {
        for (uint8_t dev = 0; dev < 32; dev++) {
            for (uint8_t func = 0; func < 8; func++) {
                uint16_t vendor = pci_read16((uint8_t)bus, dev, func, 0x00);
                if (vendor == 0xFFFF) {
                    if (func == 0) break; // no device at func0 => no funcs
                    continue;
                }

                uint16_t device = pci_read16((uint8_t)bus, dev, func, 0x02);
                uint8_t class_code = pci_read8((uint8_t)bus, dev, func, 0x0B);
                uint8_t subclass   = pci_read8((uint8_t)bus, dev, func, 0x0A);
                uint8_t prog_if    = pci_read8((uint8_t)bus, dev, func, 0x09);

                const char* cname = pci_class_name(class_code, subclass, prog_if);

                log_infof("pci",
                          "PCI %x:%x.%u vendor=%x device=%x class=%x:%x progIF=%x (%s)",
                          (unsigned)bus, (unsigned)dev, (unsigned)func,
                          vendor, device, class_code, subclass, prog_if, cname);

                // If this is an AHCI controller, print BAR5 (AHCI MMIO base)
                if (class_code == 0x01 && subclass == 0x06 && prog_if == 0x01) {
                    // AHCI needs DMA, so ensure PCI bus mastering is enabled.
                    pci_enable_bus_master((uint8_t)bus, dev, func);

                    uint32_t bar5 = pci_read_bar32((uint8_t)bus, dev, func, 5);

                    // For MMIO BARs, low bits are flags. Mask them off.
                    // (AHCI BAR5 is typically a 32-bit MMIO BAR in QEMU)
                    uint32_t mmio = bar5 & ~0x0Fu;

                    log_infof("ahci",
                              "AHCI at %x:%x.%u BAR5=%x mmio=%x",
                              (unsigned)bus, (unsigned)dev, (unsigned)func,
                              bar5, mmio);
                    ahci_probe_mmio(mmio);

                }
            }
        }
    }

    log_info("pci", "PCI scan complete");
}
