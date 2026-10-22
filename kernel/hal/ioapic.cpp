// kernel/hal/ioapic.cpp — A10c Phase 1: IO-APIC Probe
//
// QEMU q35: IO-APIC MMIO @ 0xFEC00000 (Standard).
// Register-Zugriff ueber IOREGSEL (Offset 0x00) + IOWIN (Offset 0x10).
//
// Phase 1 = nur Probe. Route/mask/unmask/eoi sind Stubs, werden in
// Phase 2 (Keyboard) aktiviert.

#include "kernel/hal/hal.h"
#include "kernel/mm/mm.h"

namespace ioapic {

static volatile u32* g_ioapic = nullptr;
static u32 g_pin_count = 0;

// Register-Indizes (in IOREGSEL geschrieben)
static constexpr u32 REG_ID    = 0x00;
static constexpr u32 REG_VER   = 0x01;
static constexpr u32 REG_TBL   = 0x10;  // Redirection Table, +2 pro GSI
static constexpr u32 REG_EOI   = 0x00;  // write-only, IOWIN

static inline u32 rd(u32 reg) {
    g_ioapic[0] = reg;              // IOREGSEL @ 0x00
    return g_ioapic[4];             // IOWIN   @ 0x10 (= index 4)
}
static inline void wr(u32 reg, u32 v) {
    g_ioapic[0] = reg;
    g_ioapic[4] = v;
}

bool is_present() { return g_ioapic != nullptr; }
u32  pin_count()  { return g_pin_count; }

void init() {
    const u64 base_phys = 0xFEC00000ULL;
    if (!vmm::map_mmio(base_phys)) {
        hal::pf("[IOAPIC] map_mmio(%p) failed\n", base_phys);
        return;
    }
    g_ioapic = (volatile u32*)mem::p2v(base_phys);

    u32 ver  = rd(REG_VER);
    u32 id   = (rd(REG_ID) >> 24) & 0x0F;
    u32 verN = ver & 0xFF;
    g_pin_count = ((ver >> 16) & 0xFF) + 1;

    hal::pf("[IOAPIC] id=%u version=%x pins=%u mmio=%p\n",
            id, verN, g_pin_count, (void*)base_phys);
}

// ── Stubs fuer Phase 2 (Keyboard-Routing) ──
void mask(u8 gsi) {
    if (!g_ioapic || gsi >= g_pin_count) return;
    u32 reg = REG_TBL + gsi * 2;
    u32 lo  = rd(reg);
    lo |= (1u << 16);               // mask bit
    wr(reg, lo);
}

void unmask(u8 gsi) {
    if (!g_ioapic || gsi >= g_pin_count) return;
    u32 reg = REG_TBL + gsi * 2;
    u32 lo  = rd(reg);
    lo &= ~(1u << 16);              // clear mask bit
    wr(reg, lo);
}

void route(u8 gsi, u8 vector, u8 dest_apic_id) {
    if (!g_ioapic || gsi >= g_pin_count) return;
    u32 reg = REG_TBL + gsi * 2;
    // lo: vector | fixed-delivery(000) | physical dest | active-high |
    //     edge-triggered | unmasked
    u32 lo = (u32)vector;
    // hi: dest APIC id in bits 24-31
    u32 hi = ((u32)dest_apic_id & 0xFF) << 24;
    wr(reg, lo);
    wr(reg + 1, hi);
}

void eoi() {
    // Fuer edge-triggered IRQs reicht Local-APIC-EOI (apic::eoi()).
    // IO-APIC-EOI ist nur fuer level-triggered (PCI) relevant.
    if (g_ioapic) wr(REG_EOI, 0);
}

} // namespace ioapic
