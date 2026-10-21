// kernel/hal/apic.cpp — A10a: Local APIC Probe + Enable
#include "kernel/hal/hal.h"
#include "kernel/mm/mm.h"

namespace apic {

static volatile u32* g_apic = nullptr;
static u64 g_tick_cycles = 1000000;

static constexpr u32 MSR_APIC_BASE = 0x1B;

static inline u32  rd(u32 off) { return g_apic[off / 4]; }
static inline void wr(u32 off, u32 v) { g_apic[off / 4] = v; }

bool is_present() { return g_apic != nullptr; }

void init() {
    u64 base_msr = hal::rdmsr(MSR_APIC_BASE);
    u64 base_phys = base_msr & 0xFFFFF000ULL;

    if (base_phys == 0) {
        hal::ps("[APIC] no APIC present (base=0)\n");
        return;
    }

    // APIC-MMIO ist nicht in der HHDM (RAM-only). Explizit mappen.
    if (!vmm::map_mmio(base_phys)) {
        hal::pf("[APIC] map_mmio(%p) failed\n", base_phys);
        return;
    }
    g_apic = (volatile u32*)mem::p2v(base_phys);

    // Global enable (Bit 11)
    base_msr |= (1ULL << 11);
    hal::wrmsr(MSR_APIC_BASE, base_msr);

    u32 id  = rd(0x20) >> 24;
    u32 ver = rd(0x30) & 0xFF;
    hal::pf("[APIC] base=%p id=%u version=0x%x\n", base_phys, id, ver);

    // SVR: APIC enable (bit 8) + spurious vector 0xFF
    wr(0xF0, 0x100 | 0xFF);
    // TPR = 0 (alle IRQs akzeptieren)
    wr(0x80, 0);

    hal::ps("[APIC] enabled\n");
}

// EOI — wird in irq_dispatch gerufen sobald Timer auf APIC laeuft.
void eoi() { if (g_apic) wr(0xB0, 0); }

void calibrate() {}
void arm(u64) {}
u64 period() { return g_tick_cycles; }

} // namespace apic
