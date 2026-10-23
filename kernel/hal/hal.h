#ifndef KERNEL_HAL_HAL_H
#define KERNEL_HAL_HAL_H

#include <stdint.h>
#include <stddef.h>

// Typedefs — gelten global (auch von nexus.cpp genutzt).
using u8  = uint8_t;  using u16 = uint16_t;  using u32 = uint32_t;
using u64 = uint64_t; using i64 = int64_t;   using usize = size_t;
using i8  = int8_t;   using i16 = int16_t;   using i32 = int32_t;

namespace hal {

// --- Port I/O ---
static inline void outb(u16 p, u8 v){ __asm__ volatile("outb %0,%1"::"a"(v),"Nd"(p)); }
static inline u8   inb(u16 p){ u8 v; __asm__ volatile("inb %1,%0":"=a"(v):"Nd"(p)); return v; }
static inline void outl(u16 p, u32 v){ __asm__ volatile("outl %0,%1"::"a"(v),"Nd"(p)); }
static inline u32  inl(u16 p){ u32 v; __asm__ volatile("inl %1,%0":"=a"(v):"Nd"(p)); return v; }
static inline void outw(u16 p, u16 v){ __asm__ volatile("outw %0,%1"::"a"(v),"Nd"(p)); }
static inline u16  inw(u16 p){ u16 v; __asm__ volatile("inw %1,%0":"=a"(v):"Nd"(p)); return v; }
static inline u64  rdmsr(u32 m){ u32 a,d; __asm__ volatile("rdmsr":"=a"(a),"=d"(d):"c"(m)); return ((u64)d<<32)|a; }
static inline void wrmsr(u32 m,u64 v){ u32 a=(u32)v,d=(u32)(v>>32); __asm__ volatile("wrmsr"::"c"(m),"a"(a),"d"(d)); }
static inline u64  read_cr2(){ u64 v; __asm__ volatile("movq %%cr2,%0":"=r"(v)); return v; }
static inline u64  read_cr3(){ u64 v; __asm__ volatile("movq %%cr3,%0":"=r"(v)); return v; }

// --- Serial-Lock (State extern, damit Inline-Lock über TUs greift) ---
extern volatile u32 g_serial_depth;
extern u64          g_serial_flags;

static inline void serial_lock() {
    if (g_serial_depth == 0)
        __asm__ volatile("pushfq; popq %0; cli" : "=r"(g_serial_flags) : : "memory");
    g_serial_depth = g_serial_depth + 1;
}
static inline void serial_unlock() {
    g_serial_depth = g_serial_depth - 1;
    if (g_serial_depth == 0 && (g_serial_flags & (1ULL<<9)))
        __asm__ volatile("sti" ::: "memory");
}

// --- Serial-API ---
void serial_init();
void serial_write_bulk_raw(const char* buf, u64 len);
void pc(char c);
void ps(const char* s);
void pf(const char* f, ...);

extern "C" volatile u32 g_shell_input_active;
bool stream_pending();
void log_flush_and_redraw(const char* prompt, const char* line, u32 len);
void echo_pc(char c);
void echo_ps(const char* s);

// --- Log-System ---
enum : u32 { LOG_DEBUG=0, LOG_INFO=1, LOG_WARN=2, LOG_ERROR=3 };
extern u32 g_log_level;
void klogf(u32 level, const char* f, ...);
void log_dump(int n);

[[noreturn]] void panic(const char* m);

} // namespace hal

// --- Spinlock ---
struct Spinlock { volatile u32 locked; };

static inline void spin_lock(Spinlock* l){
    while (__atomic_exchange_n(&l->locked, 1, __ATOMIC_ACQUIRE)) {
        while (__atomic_load_n(&l->locked, __ATOMIC_RELAXED))
            __asm__ volatile("pause");
    }
}
static inline void spin_unlock(Spinlock* l){
    __atomic_store_n(&l->locked, 0, __ATOMIC_RELEASE);
}
static inline u64 spin_lock_irqsave(Spinlock* l){
    u64 rflags;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(rflags) : : "memory");
    while (__atomic_exchange_n(&l->locked, 1, __ATOMIC_ACQUIRE)) {
        while (__atomic_load_n(&l->locked, __ATOMIC_RELAXED))
            __asm__ volatile("pause");
    }
    return rflags;
}
static inline void spin_unlock_irqrestore(Spinlock* l, u64 rflags){
    __atomic_store_n(&l->locked, 0, __ATOMIC_RELEASE);
    if (rflags & (1ULL<<9)) __asm__ volatile("sti" ::: "memory");
}

// --- PIC ---
namespace pic {
void remap();
void unmask(u8 irq);
void mask(u8 irq);
void eoi(u8 irq);
}

// --- PIT ---
namespace pit {
void init_hz(u32 hz);
}

// --- APIC (Stub, später) ---
namespace apic {
void init();
bool is_present();
void eoi();
void calibrate();
void arm(u64);
u64 period();
}


namespace ioapic {
bool is_present();
void dump_redtbl(u8 gsi_start, u8 gsi_end);
u32  pin_count();
void init();
void route(u8 gsi, u8 vector, u8 dest_apic_id);
void mask(u8 gsi);
void unmask(u8 gsi);
void eoi();
}
#endif
