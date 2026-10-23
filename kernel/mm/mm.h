// kernel/mm/mm.h — A12 Session 3.
#ifndef KERNEL_MM_MM_H
#define KERNEL_MM_MM_H

#include "kernel/hal/hal.h"

namespace mem {
extern u64 hhdm;
static inline void* p2v(u64 p){ return (void*)(p + hhdm); }
struct Region { u64 base; u64 length; };

namespace pmm {
static constexpr u64 PS  = 4096;
static constexpr u64 MAX = 1ULL<<20;
void init(const Region* regions, u32 n);
u64  alloc_n(u64 n);
u64  alloc();
void free_n(u64 base, u64 n);
u64  total();
u64  used_count();
u64  free_count();
}
}

// slab, heap, vmm sind globale Namespaces (wie im Original).
// nexus.cpp nutzt sie unqualifiziert.

namespace slab {
static constexpr int NCLASSES = 8;   // 32,64,128,256,512,1024,2048,4096
void* alloc(usize requested);
void  free(void* p);
u64   total();
u64   used();
u64   bytes_used();
void  dump();
void  trim();
}

namespace heap {
void  init();
void* alloc(usize n, usize al = 16);
void  free(void* p);
u64   used();
u64   total();
}

namespace vmm {
static constexpr u64 P_PRESENT = 1ULL<<0;
static constexpr u64 P_WRITE   = 1ULL<<1;
static constexpr u64 P_USER    = 1ULL<<2;
u64  get_phys(void* va);
bool map_user_page(u64 va, u64 phys, u64 extra_flags);
bool map_user_page_in(u64 pml4_phys, u64 va, u64 phys, u64 extra_flags);
u64  alloc_pml4();
void free_pml4(u64 phys);
u64  pte_phys_in(u64 pml4_phys, u64 va);
bool user_range_ok(u64 va, u64 len, bool need_write);
void free_user_all(u64 pml4_phys);
void self_test_pml4();
bool map_mmio(u64 phys);
}

#endif
