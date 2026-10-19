// kernel/dev/blockcache.h
#ifndef KERNEL_DEV_BLOCKCACHE_H
#define KERNEL_DEV_BLOCKCACHE_H
#include "kernel/hal/hal.h"
namespace blockcache {
int read(u64 lba, void* buf); int write(u64 lba, const void* buf);
int flush(); void invalidate_all();
u64 hits(); u64 misses(); u64 evicted();
int dirty_count(); int valid_count();
}
#endif
