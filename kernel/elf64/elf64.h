// kernel/elf64/elf64.h — Refactor Session 10a
#ifndef KERNEL_ELF64_ELF64_H
#define KERNEL_ELF64_ELF64_H

#include "kernel/hal/hal.h"

namespace elf64 {

bool validate(const u8* data, u64 size);
int  load_into(u64 pml4_phys, const u8* data, u64 size, u64* out_entry);

} // namespace elf64

#endif
