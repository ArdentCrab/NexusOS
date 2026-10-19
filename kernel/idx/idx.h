// kernel/idx/idx.h — Refactor Session 4
#ifndef KERNEL_IDX_IDX_H
#define KERNEL_IDX_IDX_H

#include "kernel/hal/hal.h"

namespace idx {

enum class Type : u8 { TASK=0, CONFIG=1, EVENT=2, TRIGGER=3, SYS=4, FILE=5, NAME=6 };

static constexpr u64 NS_CONFIG  = 0x01ULL << 56;
static constexpr u64 NS_TASK    = 0x02ULL << 56;
static constexpr u64 NS_EVENT   = 0x03ULL << 56;
static constexpr u64 NS_TRIGGER = 0x04ULL << 56;
static constexpr u64 NS_FILE    = 0x05ULL << 56;
static constexpr u64 NS_NAME    = 0x06ULL << 56;

struct Rec { u64 key; u32 type; u32 color; u64 a; u64 b; Rec* left; Rec* right; };

void  put(u64 key, Type t, u64 a, u64 b);
Rec*  find(u64 key);
bool  remove(u64 key);

using Visitor = void(*)(Rec*, void*);
void  scan(Type t, Visitor v, void* ctx);
void  scan_all(Visitor v, void* ctx);

u64  size();
Rec* debug_root();

u64  compute_hash(u32 mode, u32 filter, u32* out_count);
u64  state_hash(u32* n);
u64  full_hash(u32* n);
u64  type_hash(u32 t, u32* n);

} // namespace idx

#endif
