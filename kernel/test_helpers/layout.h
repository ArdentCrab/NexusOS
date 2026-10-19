// kernel/test_helpers/layout.h — Refactor Session 10b
#ifndef KERNEL_TEST_HELPERS_LAYOUT_H
#define KERNEL_TEST_HELPERS_LAYOUT_H

#include "kernel/hal/hal.h"

static constexpr u64 USER_CODE_VA  = 0x0000000040000000ULL;   // 1 GiB, PML4[0]
static constexpr u64 USER_STACK_VA = 0x0000000050000000ULL;   // 1.25 GiB, PML4[0]
static constexpr u64 USER_ELF_BASE = 0x0000000000400000ULL;   // Linux-Default

#endif
