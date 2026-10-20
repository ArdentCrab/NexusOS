// kernel/ipc/ipc.h — A9 Pub/Sub
#ifndef KERNEL_IPC_IPC_H
#define KERNEL_IPC_IPC_H

#include "kernel/hal/hal.h"

namespace ipc {

static constexpr u32 SUB_MAX = 8;

struct Subscription {
    u64  type_filter;   // 0 = alle
    u64  last_seq;
    bool used;
};

// Globale API (kernel-intern, syscalls rufen diese)
u64  next_seq();
void publish(u64 user_key, u8 user_type, u64 a, u64 b);

// Per-Task-Subs, ueber TCB::subs[]
i64  sub_alloc(Subscription* subs, u64 type_filter);
i64  sub_unsubscribe(Subscription* subs, u64 handle);
// Wartet auf naechstes Event. timeout_ms=0: non-blocking.
// Rueckgabe: 0 = Event, -EAGAIN, -EINVAL.
i64  wait_event(Subscription* subs, u64 handle, u64 timeout_ms,
                u64* out_key, u64* out_a);

} // namespace ipc

#endif
