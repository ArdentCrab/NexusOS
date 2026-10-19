// kernel/main/main.h — Refactor Session 11c-light
#ifndef KERNEL_MAIN_MAIN_H
#define KERNEL_MAIN_MAIN_H

#include "kernel/hal/hal.h"

// Nicht-static Symbole (kernel_main ruft sie in nexus.cpp)
void init_fn(void*);
void idle_fn(void*);
void trig_timer(u64 kind, u64 a, u64 b);
void wal_apply(u8 op, u8 type, u64 key, u64 a, u64 b);

#endif
