// kernel/hal/apic.cpp — A10b: Local APIC + Timer
#include "kernel/hal/hal.h"
#include "kernel/mm/mm.h"

namespace apic {

static volatile u32* g_apic = nullptr;
static u64 g_tick_cycles = 62500;  // QEMU: 100 MHz / 16 / 100 Hz

// QEMU-Model: Local APIC Timer laeuft mit 100 MHz Bus-Frequency.
// Auf echter Hardware muss das kalibriert werden (A10d).
static constexpr u64 APIC_BUS_FREQ_HZ = 100000000;
static constexpr u32 TIMER_DIVIDE     = 16;

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

    if (!vmm::map_mmio(base_phys)) {
        hal::pf("[APIC] map_mmio(%p) failed\n", base_phys);
        return;
    }
    g_apic = (volatile u32*)mem::p2v(base_phys);

    base_msr |= (1ULL << 11);
    hal::wrmsr(MSR_APIC_BASE, base_msr);

    u32 id  = rd(0x20) >> 24;
    u32 ver = rd(0x30) & 0xFF;
    hal::pf("[APIC] base=%p id=%u version=0x%x\n", base_phys, id, ver);

    wr(0xF0, 0x100 | 0xFF);  // SVR: enable + spurious 0xFF
    wr(0x80, 0);             // TPR = 0

    hal::ps("[APIC] enabled\n");
}

// Arm periodic APIC timer. hz = Ziel-Frequenz.
void arm(u64 hz) {
    if (!g_apic) { hal::ps("[APIC] arm skipped (not present)\n"); return; }

    u64 ticks = APIC_BUS_FREQ_HZ / (hz * TIMER_DIVIDE);
    g_tick_cycles = ticks;

    // Divide Configuration (0x3E0): 0x3 = divide by 16
    wr(0x3E0, 0x3);
    // Initial Count (0x380)
    wr(0x380, (u32)ticks);
    // LVT Timer (0x320): periodic (bit 17) | vector 32
    wr(0x320, (1 << 17) | 32);

    hal::pf("[APIC] timer armed: %u Hz, divide=%u, init_count=%u\n",
            hz, TIMER_DIVIDE, (u32)ticks);
}

// Send EOI to Local APIC (write 0 to EOI register).
void eoi() { if (g_apic) wr(0xB0, 0); }

void calibrate() {}
u64 period() { return g_tick_cycles; }

} // namespace apic
