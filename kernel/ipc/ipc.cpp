// kernel/ipc/ipc.cpp — A9 Pub/Sub
#include "kernel/ipc/ipc.h"
#include "kernel/idx/idx.h"

extern volatile u64 g_tick;  // global, definiert in nexus.cpp

namespace ipc {

static u64 g_next_seq = 1;

u64 next_seq() { return g_next_seq; }

void publish(u64 user_key, u8 user_type, u64 a, u64 b) {
    // pub_key = NS_EVENT | (seq << 8) | (user_type & 0xFF)
    // b_val wird nicht genutzt (Rec hat nur a, b: key + a_val)
    // Convention: user_key in a, user_payload_a in b
    u64 seq = g_next_seq++;
    u64 pub_key = idx::NS_EVENT | ((seq & 0x00FFFFFFFFFFFFFFULL) << 8) | (user_type & 0xFF);
    idx::put(pub_key, idx::Type::EVENT, user_key, a);
    (void)b;
}

i64 sub_alloc(Subscription* subs, u64 type_filter) {
    for (u32 i = 0; i < SUB_MAX; i++) {
        if (!subs[i].used) {
            subs[i].type_filter = type_filter;
            subs[i].last_seq = 0;
            subs[i].used = true;
            return (i64)i;
        }
    }
    return -24;  // EMFILE
}

i64 sub_unsubscribe(Subscription* subs, u64 handle) {
    if (handle >= SUB_MAX) return -22;
    if (!subs[handle].used) return -22;
    subs[handle].used = false;
    return 0;
}

struct WaitCtx {
    u64 type_filter;
    u64 last_seq;
    u64 found_key;
    u64 found_a;
    u64 found_seq;
};

static void wait_visit(idx::Rec* r, void* v) {
    WaitCtx* c = (WaitCtx*)v;
    if (r->type != (u32)idx::Type::EVENT) return;
    u64 seq = (r->key & 0x00FFFFFFFFFFFFFFULL) >> 8;   // NS maskieren, DANN shiften
    u8  ut  = (u8)(r->key & 0xFF);
    if (seq <= c->last_seq) return;
    if (c->type_filter != 0 && (u64)ut != c->type_filter) return;
    if (c->found_seq != 0 && seq >= c->found_seq) return;
    c->found_seq = seq;
    c->found_key = r->a;
    c->found_a   = r->b;
}

i64 wait_event(Subscription* subs, u64 handle, u64 timeout_ms,
               u64* out_key, u64* out_a) {
    if (handle >= SUB_MAX) return -22;
    if (!subs[handle].used) return -22;
    u64 deadline = (timeout_ms == 0) ? 0 : (g_tick + (timeout_ms / 10));
    for (;;) {
        WaitCtx c = { subs[handle].type_filter, subs[handle].last_seq, 0, 0, 0 };
        idx::scan(idx::Type::EVENT, wait_visit, &c);
        if (c.found_seq != 0) {
            subs[handle].last_seq = c.found_seq;
            *out_key = c.found_key;
            *out_a   = c.found_a;
            return 0;
        }
        if (timeout_ms == 0) return -11;   // EAGAIN
        if (g_tick >= deadline) return -110; // ETIMEDOUT
        // B1-TEMP: event-wait disabled
        return -110;
    }
}

} // namespace ipc
