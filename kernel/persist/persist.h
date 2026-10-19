// kernel/persist/persist.h — Refactor Session 6
#ifndef KERNEL_PERSIST_PERSIST_H
#define KERNEL_PERSIST_PERSIST_H

#include "kernel/hal/hal.h"

namespace persist {

static constexpr u32 WAL_START = 1024;

bool wal_in_recovery();
void wal_set_recovery(bool b);
u32  wal_head();
u32  wal_count();
u64  snap_seq();
u64  puts_since_sync();
bool autosync_pending();
void request_autosync();
int  autosync_now();

int  save();
int  load();
int  wal_replay();
void wal_reset();
void wal_append_put(u64 key, u32 type, u64 a, u64 b);
void wal_append_remove(u64 key);

using WalApplyFn = void(*)(u8 op, u8 type, u64 key, u64 a, u64 b);
void wal_set_apply(WalApplyFn fn);

using WalIdxVisitor = void (*)(u64 index, u8 op, u8 type, u64 key,
                               u64 a, u64 b, void* ctx);
int  wal_iterate_indexed(WalIdxVisitor v, void* ctx);

int  checkpoint_create(const char* name);
int  checkpoint_restore(const char* name);
int  checkpoint_count();

} // namespace persist

#endif
