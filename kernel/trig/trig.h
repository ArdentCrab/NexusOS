// kernel/trig/trig.h — Refactor Session 11a
#ifndef KERNEL_TRIG_TRIG_H
#define KERNEL_TRIG_TRIG_H

#include "kernel/hal/hal.h"

namespace trig {
using Fn = void(*)(u64, u64, u64);
void on(u64 kind, Fn fn);
void emit(u64 kind, u64 a, u64 b);
} // namespace trig

#endif
