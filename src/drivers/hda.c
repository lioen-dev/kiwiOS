// src/drivers/hda.c

#include "drivers/hda.h"
#include "drivers/pci.h"
#include "memory/vmm.h"   // phys_to_virt, vmm_map_page, vmm_get_kernel_page_table
#include "memory/pmm.h"   // pmm_alloc: returns physical address as void*
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// PCI class codes for High Definition Audio
#define HDA_PCI_CLASS    0x04  // Multimedia
#define HDA_PCI_SUBCLASS 0x03  // HD Audio

// HDA controller register offsets
#define HDA_REG_GCAP     0x00  // Global Capabilities (16-bit)
#define HDA_REG_VMIN     0x02  // Minor version (8-bit)
#define HDA_REG_VMAJ     0x03  // Major version (8-bit)
#define HDA_REG_GCTL     0x08  // Global Control (32-bit)
#define HDA_REG_STATESTS 0x0E  // State Change Status (16-bit)

// CORB registers
#define HDA_REG_CORBLBASE 0x40 // CORB Lower Base Address (32-bit)
#define HDA_REG_CORBUBASE 0x44 // CORB Upper Base Address (32-bit)
#define HDA_REG_CORBWP    0x48 // CORB Write Pointer (8-bit)
#define HDA_REG_CORBRP    0x4A // CORB Read Pointer (16-bit)
#define HDA_REG_CORBCTL   0x4C // CORB Control (8-bit)
#define HDA_REG_CORBSTS   0x4D // CORB Status (8-bit)
#define HDA_REG_CORBSIZE  0x4E // CORB Size (8-bit)

// RIRB registers
#define HDA_REG_RIRBLBASE 0x50 // RIRB Lower Base Address (32-bit)
#define HDA_REG_RIRBUBASE 0x54 // RIRB Upper Base Address (32-bit)
#define HDA_REG_RIRBWP    0x58 // RIRB Write Pointer (16-bit)
#define HDA_REG_RINTCNT   0x5A // Response Interrupt Count (16-bit)
#define HDA_REG_RIRBCTL   0x5C // RIRB Control (8-bit)
#define HDA_REG_RIRBSTS   0x5D // RIRB Status (8-bit)
#define HDA_REG_RIRBSIZE  0x5E // RIRB Size (8-bit)

// Immediate Command registers
#define HDA_REG_IC       0x60  // Immediate Command Output (32-bit)
#define HDA_REG_IR       0x64  // Immediate Response Input (32-bit)
#define HDA_REG_ICS      0x68  // Immediate Command Status (16-bit)

// ICS bits
#define HDA_ICS_ICB      (1u << 0)  // Immediate Command Busy
#define HDA_ICS_IRV      (1u << 1)  // Immediate Response Valid

// GCTL bits
#define HDA_GCTL_CRST    (1u << 0)  // Controller Reset
#define HDA_GCTL_UNSOL   (1u << 8)  // Accept unsolicited responses (some HW needs this for codec 0) 

// CORBCTL bits
#define HDA_CORBCTL_DMA_EN   (1u << 1)

// RIRBCTL bits
#define HDA_RIRBCTL_DMA_EN   (1u << 0)

// CORBRP bits
#define HDA_CORBRP_RST       (1u << 15)

// RIRBWP bits
#define HDA_RIRBWP_RST       (1u << 15)

// Common verbs/parameters
#define HDA_VERB_GET_PARAMETER  0xF00

// GetParameter parameter IDs
#define HDA_PARAM_VENDOR_ID     0x00
#define HDA_PARAM_NODE_COUNT    0x04

// HDA controller state
typedef struct {
    bool present;

    uint8_t bus;
    uint8_t slot;
    uint8_t func;

    volatile uint8_t* mmio_base;

    uint16_t gcap;
    uint8_t  vmaj;
    uint8_t  vmin;

    bool     reset_ok;

    // Codec presence
    uint16_t codec_mask;       // raw STATESTS bits
    bool     codec_present;    // at least one codec?
    uint8_t  primary_codec;    // "best" codec we picked

    // CORB/RIRB DMA buffers (physical + virtual)
    uint64_t           corb_phys;
    uint64_t           rirb_phys;
    volatile uint32_t* corb_virt;   // CORB entries (32-bit)
    volatile uint64_t* rirb_virt;   // RIRB entries (64-bit)
    uint16_t           corb_entries;
    uint16_t           rirb_entries;
    uint8_t            corb_wp;     // last written CORB index
    uint16_t           rirb_rp;     // last consumed RIRB index
    bool               corb_rirb_ok;
} hda_controller_t;

static hda_controller_t g_hda = {0};


// -------------------- MMIO helpers --------------------

static inline uint8_t hda_mmio_read8(size_t offset) {
    return *(volatile uint8_t *)(g_hda.mmio_base + offset);
}

static inline uint16_t hda_mmio_read16(size_t offset) {
    return *(volatile uint16_t *)(g_hda.mmio_base + offset);
}

static inline uint32_t hda_mmio_read32(size_t offset) {
    return *(volatile uint32_t *)(g_hda.mmio_base + offset);
}

static inline void hda_mmio_write8(size_t offset, uint8_t value) {
    *(volatile uint8_t *)(g_hda.mmio_base + offset) = value;
}

static inline void hda_mmio_write16(size_t offset, uint16_t value) {
    *(volatile uint16_t *)(g_hda.mmio_base + offset) = value;
}

static inline void hda_mmio_write32(size_t offset, uint32_t value) {
    *(volatile uint32_t *)(g_hda.mmio_base + offset) = value;
}


// -------------------- Verb builder --------------------

// Verb format: [31:28] codec, [27:20] node, [19:8] verb, [7:0] payload
static inline uint32_t hda_make_verb(uint8_t codec, uint8_t node,
                                     uint16_t verb, uint16_t payload) {
    return ((uint32_t)codec  << 28) |
           ((uint32_t)node   << 20) |
           ((uint32_t)verb   << 8)  |
           (payload & 0xFFu);
}


// -------------------- Immediate Command Sender --------------------

static bool hda_send_verb_immediate(uint8_t codec, uint8_t node,
                                    uint16_t verb, uint16_t payload,
                                    uint32_t* out_resp) {
    if (!g_hda.present || !g_hda.mmio_base) return false;

    // 1) Wait until controller idle (ICB clear)
    for (int i = 0; i < 1000000; i++) {
        uint16_t ics = hda_mmio_read16(HDA_REG_ICS);
        if ((ics & HDA_ICS_ICB) == 0)
            break;
        if (i == 999999)
            return false; // timeout waiting for idle
    }

    // 2) Write verb to IC
    uint32_t icw = hda_make_verb(codec, node, verb, payload);
    hda_mmio_write32(HDA_REG_IC, icw);

    // 3) Set ICB to launch command
    uint16_t ics = hda_mmio_read16(HDA_REG_ICS);
    ics |= HDA_ICS_ICB;
    hda_mmio_write16(HDA_REG_ICS, ics);

    // 4) Wait for response (ICB=0 and IRV=1)
    for (int i = 0; i < 1000000; i++) {
        ics = hda_mmio_read16(HDA_REG_ICS);
        if ((ics & HDA_ICS_ICB) == 0 && (ics & HDA_ICS_IRV)) {
            uint32_t resp = hda_mmio_read32(HDA_REG_IR);

            // Clear IRV
            hda_mmio_write16(HDA_REG_ICS, HDA_ICS_IRV);

            if (out_resp) *out_resp = resp;
            return true;
        }
    }

    // Timed out: clear ICB and wait for it to clear
    ics = hda_mmio_read16(HDA_REG_ICS);
    ics &= ~HDA_ICS_ICB;
    hda_mmio_write16(HDA_REG_ICS, ics);

    for (int i = 0; i < 1000000; i++) {
        if ((hda_mmio_read16(HDA_REG_ICS) & HDA_ICS_ICB) == 0)
            break;
    }

    return false; // timeout / no response
}


// -------------------- Reset & Codec Detection --------------------

static bool hda_reset_controller(void) {
    // Clear CRST
    uint32_t gctl = hda_mmio_read32(HDA_REG_GCTL);
    gctl &= ~HDA_GCTL_CRST;
    hda_mmio_write32(HDA_REG_GCTL, gctl);

    for (int i = 0; i < 1000000; i++) {
        if (!(hda_mmio_read32(HDA_REG_GCTL) & HDA_GCTL_CRST))
            break;
        if (i == 999999)
            return false;
    }

    // Set CRST
    gctl = hda_mmio_read32(HDA_REG_GCTL) | HDA_GCTL_CRST;
    hda_mmio_write32(HDA_REG_GCTL, gctl);

    for (int i = 0; i < 1000000; i++) {
        if (hda_mmio_read32(HDA_REG_GCTL) & HDA_GCTL_CRST)
            return true;
    }

    return false;
}

static void hda_discover_codecs(void) {
    g_hda.codec_mask    = 0;
    g_hda.codec_present = false;
    g_hda.primary_codec = 0;

    uint16_t mask = 0;

    // After reset, codecs assert STATESTS bits
    for (int i = 0; i < 100000; i++) {
        mask = hda_mmio_read16(HDA_REG_STATESTS);
        if (mask != 0)
            break;
    }

    g_hda.codec_mask = mask;

    if (mask == 0) {
        g_hda.codec_present = false;
        return;
    }

    // For now just mark "present"; we'll pick the real primary later
    g_hda.codec_present = true;
    g_hda.primary_codec = 0; // temporary default; may be overwritten
}


// -------------------- CORB/RIRB init --------------------

// For now: 16 entries for CORB and 16 for RIRB,
// one 4KB page each from the PMM, DMA enabled, polled usage.

static bool hda_init_corb_rirb(void) {
    if (!g_hda.present || !g_hda.mmio_base) return false;

    // 1) Disable CORB/RIRB DMA while we configure
    uint8_t corbctl = hda_mmio_read8(HDA_REG_CORBCTL);
    corbctl &= ~HDA_CORBCTL_DMA_EN;
    hda_mmio_write8(HDA_REG_CORBCTL, corbctl);

    uint8_t rirbctl = hda_mmio_read8(HDA_REG_RIRBCTL);
    rirbctl &= ~HDA_RIRBCTL_DMA_EN;
    hda_mmio_write8(HDA_REG_RIRBCTL, rirbctl);

    // 2) Allocate one page each for CORB and RIRB from the PMM.
    void* corb_page = pmm_alloc();
    if (!corb_page) return false;
    uint64_t corb_phys = (uint64_t)(uintptr_t)corb_page;

    void* rirb_page = pmm_alloc();
    if (!rirb_page) return false;
    uint64_t rirb_phys = (uint64_t)(uintptr_t)rirb_page;

    g_hda.corb_phys = corb_phys;
    g_hda.rirb_phys = rirb_phys;

    // Map them into virtual space via HHDM so CPU can write/read.
    g_hda.corb_virt = (volatile uint32_t*)phys_to_virt(corb_phys);
    g_hda.rirb_virt = (volatile uint64_t*)phys_to_virt(rirb_phys);

    g_hda.corb_entries = 16;
    g_hda.rirb_entries = 16;
    g_hda.corb_wp      = 0;
    g_hda.rirb_rp      = 0;

    // 3) Program CORB base
    hda_mmio_write32(HDA_REG_CORBLBASE, (uint32_t)(corb_phys & 0xFFFFFFFF));
    hda_mmio_write32(HDA_REG_CORBUBASE, (uint32_t)(corb_phys >> 32));

    // 4) Program RIRB base
    hda_mmio_write32(HDA_REG_RIRBLBASE, (uint32_t)(rirb_phys & 0xFFFFFFFF));
    hda_mmio_write32(HDA_REG_RIRBUBASE, (uint32_t)(rirb_phys >> 32));

    // 5) Set CORB size to 16 entries (code 0x1)
    uint8_t corbsize = hda_mmio_read8(HDA_REG_CORBSIZE);
    corbsize &= ~0x3u;
    corbsize |= 0x1u;   // 16 entries
    hda_mmio_write8(HDA_REG_CORBSIZE, corbsize);

    // 6) Set RIRB size to 16 entries (code 0x1)
    uint8_t rirbsize = hda_mmio_read8(HDA_REG_RIRBSIZE);
    rirbsize &= ~0x3u;
    rirbsize |= 0x1u;
    hda_mmio_write8(HDA_REG_RIRBSIZE, rirbsize);

    // 7) Reset CORB read pointer
    hda_mmio_write16(HDA_REG_CORBRP, HDA_CORBRP_RST);
    hda_mmio_write16(HDA_REG_CORBRP, 0);

    // 8) Clear CORB write pointer
    hda_mmio_write8(HDA_REG_CORBWP, 0);
    g_hda.corb_wp = 0;

    // 9) Reset RIRB write pointer
    hda_mmio_write16(HDA_REG_RIRBWP, HDA_RIRBWP_RST);
    hda_mmio_write16(HDA_REG_RIRBWP, 0);
    g_hda.rirb_rp = 0;

    // 10) Set response interrupt count (we're polling, but set to 1)
    hda_mmio_write16(HDA_REG_RINTCNT, 1);

    // 11) Clear status bits
    hda_mmio_write8(HDA_REG_CORBSTS, hda_mmio_read8(HDA_REG_CORBSTS));
    hda_mmio_write8(HDA_REG_RIRBSTS, hda_mmio_read8(HDA_REG_RIRBSTS));

    // 12) Enable CORB and RIRB DMA (interrupts still off)
    corbctl = hda_mmio_read8(HDA_REG_CORBCTL);
    corbctl |= HDA_CORBCTL_DMA_EN;
    hda_mmio_write8(HDA_REG_CORBCTL, corbctl);

    rirbctl = hda_mmio_read8(HDA_REG_RIRBCTL);
    rirbctl |= HDA_RIRBCTL_DMA_EN;
    hda_mmio_write8(HDA_REG_RIRBCTL, rirbctl);

    return true;
}


// -------------------- CORB-based verb sender --------------------

static bool hda_send_verb_corb(uint8_t codec, uint8_t node,
                               uint16_t verb, uint16_t payload,
                               uint32_t* out_resp) {
    if (!g_hda.corb_rirb_ok) return false;
    if (!g_hda.corb_virt || !g_hda.rirb_virt) return false;

    uint16_t corb_entries = g_hda.corb_entries;
    uint16_t rirb_entries = g_hda.rirb_entries;
    if (corb_entries == 0 || rirb_entries == 0) return false;

    // one outstanding command at a time

    // 1) Compute next CORB index and write the verb there
    uint8_t new_wp = (uint8_t)((g_hda.corb_wp + 1) & (corb_entries - 1));
    uint32_t cmd   = hda_make_verb(codec, node, verb, payload);

    g_hda.corb_virt[new_wp] = cmd;
    g_hda.corb_wp = new_wp;

    // 2) Notify hardware of new write pointer
    hda_mmio_write8(HDA_REG_CORBWP, new_wp);

    // 3) Poll RIRBWP until it advances
    uint16_t old_rp = g_hda.rirb_rp;

    for (int i = 0; i < 1000000; i++) {
        uint16_t hw_wp = hda_mmio_read16(HDA_REG_RIRBWP) & 0xFFu;
        if (hw_wp != old_rp) {
            // New response available at index hw_wp
            g_hda.rirb_rp = hw_wp;
            uint16_t idx  = (uint16_t)(hw_wp & (rirb_entries - 1));

            uint64_t entry = g_hda.rirb_virt[idx];
            uint32_t resp  = (uint32_t)(entry & 0xFFFFFFFFu);

            // Clear any RIRB status bits by writing back what we read
            uint8_t sts = hda_mmio_read8(HDA_REG_RIRBSTS);
            if (sts) {
                hda_mmio_write8(HDA_REG_RIRBSTS, sts);
            }

            if (out_resp) *out_resp = resp;
            return true;
        }
    }

    return false; // timeout waiting for RIRB response
}


// -------------------- Public API: init --------------------

bool hda_init(void) {
    pci_device_t dev;

    // 1) PCI detect
    if (!pci_find_class_subclass(HDA_PCI_CLASS, HDA_PCI_SUBCLASS, &dev)) {
        g_hda.present = false;
        return false;
    }

    // 2) Read BAR0
    uint32_t bar0 = pci_config_read32(dev.bus, dev.slot, dev.func, 0x10);
    if (bar0 & 1) { // IO-space BAR? shouldn't happen for HDA
        g_hda.present = false;
        return false;
    }

    uint64_t phys_base = (uint64_t)(bar0 & ~0xFu);

    // 3) Enable MMIO + Bus Master
    pci_enable_mmio_and_bus_mastering(dev.bus, dev.slot, dev.func);

    // 4) Map MMIO page into kernel page tables
    page_table_t* kpt = vmm_get_kernel_page_table();
    void* mmio_virt = phys_to_virt(phys_base);

    vmm_map_page(kpt,
                 (uint64_t)mmio_virt,
                 phys_base,
                 PAGE_PRESENT | PAGE_WRITE);

    // 5) Store info
    g_hda.present   = true;
    g_hda.bus       = dev.bus;
    g_hda.slot      = dev.slot;
    g_hda.func      = dev.func;
    g_hda.mmio_base = (volatile uint8_t*)mmio_virt;

    // 6) Read core registers
    g_hda.gcap = hda_mmio_read16(HDA_REG_GCAP);
    g_hda.vmin = hda_mmio_read8(HDA_REG_VMIN);
    g_hda.vmaj = hda_mmio_read8(HDA_REG_VMAJ);

    // 7) Reset
    g_hda.reset_ok = hda_reset_controller();

    // 8) Discover codecs
    if (g_hda.reset_ok) {
        hda_discover_codecs();
    } else {
        g_hda.codec_present = false;
        g_hda.codec_mask    = 0;
    }

    // 9) Initialize CORB/RIRB DMA rings
    g_hda.corb_rirb_ok = hda_init_corb_rirb();

    // 10) Enable unsolicited responses (some HW requires this for proper codec responses) 
    if (g_hda.reset_ok) {
        uint32_t gctl = hda_mmio_read32(HDA_REG_GCTL);
        gctl |= HDA_GCTL_UNSOL;
        hda_mmio_write32(HDA_REG_GCTL, gctl);
    }

    return true;
}


// -------------------- Public API: getters --------------------

bool hda_is_present(void) {
    return g_hda.present;
}

uint16_t hda_get_gcap(void) {
    return g_hda.gcap;
}

uint8_t hda_get_version_major(void) {
    return g_hda.vmaj;
}

uint8_t hda_get_version_minor(void) {
    return g_hda.vmin;
}

bool hda_controller_was_reset(void) {
    return g_hda.reset_ok;
}

bool hda_has_codec(void) {
    return g_hda.codec_present;
}

uint8_t hda_get_primary_codec_id(void) {
    return g_hda.primary_codec;
}

uint16_t hda_get_codec_mask(void) {
    return g_hda.codec_mask;
}

bool hda_corb_rirb_ready(void) {
    return g_hda.corb_rirb_ok;
}


// -------------------- Public API: codec queries --------------------

// Try to talk to each present codec and pick the first one that replies.
// This is more robust than assuming codec 0 will always respond.
bool hda_get_codec0_vendor_immediate(uint32_t* out_vendor) {
    if (!out_vendor) return false;
    if (!g_hda.codec_present) return false;
    if (!g_hda.codec_mask) return false;

    uint16_t mask = g_hda.codec_mask;
    uint32_t resp = 0;

    // 1) First pass: use CORB/RIRB for any codec that has a bit set.
    if (g_hda.corb_rirb_ok) {
        for (uint8_t c = 0; c < 15; c++) {
            if (!(mask & (1u << c)))
                continue;

            if (hda_send_verb_corb(c, 0,
                                   HDA_VERB_GET_PARAMETER,
                                   HDA_PARAM_VENDOR_ID,
                                   &resp)) {
                g_hda.primary_codec = c;
                *out_vendor = resp;
                return true;
            }
        }
    }

    // 2) Second pass: fall back to Immediate Command for each codec.
    for (uint8_t c = 0; c < 15; c++) {
        if (!(mask & (1u << c)))
            continue;

        if (hda_send_verb_immediate(c, 0,
                                    HDA_VERB_GET_PARAMETER,
                                    HDA_PARAM_VENDOR_ID,
                                    &resp)) {
            g_hda.primary_codec = c;
            *out_vendor = resp;
            return true;
        }
    }

    // If we get here, none of the codecs replied.
    return false;
}

static bool hda_try_nodecount_on_nid(uint8_t codec, uint8_t nid, uint32_t* out_resp) {
    uint32_t resp = 0;

    // 1) CORB/RIRB path
    if (g_hda.corb_rirb_ok) {
        if (hda_send_verb_corb(codec, nid,
                               HDA_VERB_GET_PARAMETER,
                               HDA_PARAM_NODE_COUNT,
                               &resp)) {
            if (out_resp) *out_resp = resp;
            return true;
        }
    }

    // 2) Immediate fallback
    if (hda_send_verb_immediate(codec, nid,
                                HDA_VERB_GET_PARAMETER,
                                HDA_PARAM_NODE_COUNT,
                                &resp)) {
        if (out_resp) *out_resp = resp;
        return true;
    }

    return false;
}

bool hda_codec0_get_sub_nodes(uint8_t parent_nid,
                              uint8_t* out_start,
                              uint8_t* out_count) {
    if (!out_start || !out_count) return false;
    if (!g_hda.codec_present) return false;
    if (!g_hda.codec_mask) return false;

    uint8_t codec = g_hda.primary_codec;
    uint32_t resp = 0;

    // First try the requested NID
    if (!hda_try_nodecount_on_nid(codec, parent_nid, &resp)) {
        // If that was NID 0 (root), fall back to NID 1 (common AFG)
        if (parent_nid == 0) {
            uint8_t fallback_nid = 1;
            if (!hda_try_nodecount_on_nid(codec, fallback_nid, &resp)) {
                return false;
            }
            parent_nid = fallback_nid; // we don't actually use parent_nid afterwards, but it's conceptually our "root"
        } else {
            return false;
        }
    }

    uint16_t start_nid  = (uint16_t)((resp >> 16) & 0x7fff);
    uint16_t node_count = (uint16_t)(resp & 0x7fff);

    *out_start = (uint8_t)start_nid;
    *out_count = (uint8_t)node_count;
    return true;
}
