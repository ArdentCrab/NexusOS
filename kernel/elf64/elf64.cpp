// kernel/elf64/elf64.cpp — Refactor Session 10a
#include "kernel/elf64/elf64.h"
#include "kernel/mm/mm.h"

namespace elf64 {

struct __attribute__((packed)) Ehdr {
    u8  e_ident[16];
    u16 e_type;
    u16 e_machine;
    u32 e_version;
    u64 e_entry;
    u64 e_phoff;
    u64 e_shoff;
    u32 e_flags;
    u16 e_ehsize;
    u16 e_phentsize;
    u16 e_phnum;
    u16 e_shentsize;
    u16 e_shnum;
    u16 e_shstrndx;
};
struct __attribute__((packed)) Phdr {
    u32 p_type;
    u32 p_flags;
    u64 p_offset;
    u64 p_vaddr;
    u64 p_paddr;
    u64 p_filesz;
    u64 p_memsz;
    u64 p_align;
};

enum : u16 { ET_EXEC = 2, EM_X86_64 = 62 };
enum : u32 { PT_LOAD = 1 };
enum : u32 { PF_X = 1, PF_W = 2, PF_R = 4 };

bool validate(const u8* data, u64 size) {
    if (size < sizeof(Ehdr)) return false;
    const Ehdr* e = (const Ehdr*)data;
    if (e->e_ident[0] != 0x7F) return false;
    if (e->e_ident[1] != 'E') return false;
    if (e->e_ident[2] != 'L') return false;
    if (e->e_ident[3] != 'F') return false;
    if (e->e_ident[4] != 2) return false;      // ELFCLASS64
    if (e->e_ident[5] != 1) return false;      // little-endian
    if (e->e_type != ET_EXEC) return false;
    if (e->e_machine != EM_X86_64) return false;
    if (e->e_phentsize != sizeof(Phdr)) return false;
    if (e->e_phnum == 0 || e->e_phnum > 32) return false;
    if (e->e_phoff + (u64)e->e_phnum * sizeof(Phdr) > size) return false;
    return true;
}

// Laedt PT_LOAD-Segmente in die gegebene PML4. Rueckgabe 0 = OK.
// Fehlercodes: -1 validate, -2 VA out of user range, -3 file bounds,
// -4 OOM, -5 map fail.
int load_into(u64 pml4_phys, const u8* data, u64 size, u64* out_entry) {
    if (!validate(data, size)) return -1;
    const Ehdr* e = (const Ehdr*)data;
    const Phdr* ph = (const Phdr*)(data + e->e_phoff);

    for (u32 i = 0; i < e->e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD) continue;
        if (ph[i].p_memsz == 0) continue;

        u64 vstart = ph[i].p_vaddr & ~0xFFFULL;
        u64 vend   = (ph[i].p_vaddr + ph[i].p_memsz + 0xFFF) & ~0xFFFULL;

        if (vstart >= 0x0000800000000000ULL) return -2;
        if (vend   >  0x0000800000000000ULL) return -2;
        if (ph[i].p_offset + ph[i].p_filesz > size) return -3;

        u64 extra = 0;
        if (ph[i].p_flags & PF_W) extra |= vmm::P_WRITE;

        for (u64 va = vstart; va < vend; va += 0x1000) {
            u64 phys = mem::pmm::alloc();
            if (!phys) return -4;
            u8* dst = (u8*)mem::p2v(phys);
            for (int k = 0; k < 4096; k++) dst[k] = 0;

            u64 lo = va > ph[i].p_vaddr ? va : ph[i].p_vaddr;
            u64 hi = va + 4096;
            u64 seg_hi = ph[i].p_vaddr + ph[i].p_filesz;
            if (hi > seg_hi) hi = seg_hi;

            if (lo < hi) {
                u64 src = ph[i].p_offset + (lo - ph[i].p_vaddr);
                u64 off = lo - va;
                u64 n   = hi - lo;
                for (u64 k = 0; k < n; k++) dst[off + k] = data[src + k];
            }
            if (!vmm::map_user_page_in(pml4_phys, va, phys, extra)) {
                mem::pmm::free_n(phys, 1);
                return -5;
            }
        }
    }
    *out_entry = e->e_entry;
    return 0;
}

} // namespace elf64
