// kernel/ipc/msg.cpp — B1.1: Index-Based Message-Passing
#include "kernel/ipc/msg.h"
#include "kernel/idx/idx.h"

extern volatile u64 g_tick;

namespace ipc {

struct MsgSlot {
    u8   data[MSG_MAX_LEN];
    u16  len;
    bool used;
};

static MsgSlot g_pool[MSG_POOL_SIZE];
static u64     g_seq = 1;

struct MsgVisitCtx {
    u8   ch;
    u64  best_key;
    u64  best_slot;
    u64  best_len;
};

static void msg_visit(idx::Rec* r, void* vp) {
    MsgVisitCtx* c = (MsgVisitCtx*)vp;
    u8 ch = (u8)(r->key & 0xFF);
    if (ch != c->ch) return;
    if (c->best_key == 0 || r->key < c->best_key) {
        c->best_key  = r->key;
        c->best_slot = r->a;
        c->best_len  = r->b;
    }
}

int msg_send(u8 channel, const u8* data, u32 len) {
    if (channel >= MSG_CHANNELS) return -22;
    if (len > MSG_MAX_LEN) return -22;
    if (len == 0) return 0;

    int slot = -1;
    for (u32 i = 0; i < MSG_POOL_SIZE; i++) {
        if (!g_pool[i].used) { slot = (int)i; break; }
    }
    if (slot < 0) return -105;   // ENOBUFS

    for (u32 i = 0; i < len; i++) g_pool[slot].data[i] = data[i];
    g_pool[slot].len = (u16)len;
    g_pool[slot].used = true;

    u64 seq = g_seq++;
    u64 key = idx::NS_MSG
            | ((seq & 0x00FFFFFFFFFFFFFFULL) << 8)
            | (channel & 0xFF);
    idx::put(key, idx::Type::MSG, (u64)slot, (u64)len);
    return 0;
}

int msg_recv(u8 channel, u8* out, u32 cap, u32 timeout_ms) {
    if (channel >= MSG_CHANNELS) return -22;
    u64 deadline = timeout_ms ? (g_tick + (timeout_ms / 10) + 1) : 0;

    for (;;) {
        MsgVisitCtx c = { channel, 0, 0, 0 };
        idx::scan(idx::Type::MSG, msg_visit, &c);

        if (c.best_key != 0) {
            u32 n = (u32)c.best_len;
            if (n > cap) n = cap;
            MsgSlot* s = &g_pool[c.best_slot];
            for (u32 i = 0; i < n; i++) out[i] = s->data[i];
            s->used = false;
            idx::remove(c.best_key);
            return (int)n;
        }

        if (timeout_ms == 0) return -11;   // EAGAIN
        if (g_tick >= deadline) return -110;   // ETIMEDOUT
        __asm__ volatile("sti; hlt");
    }
}

void msg_stats(u32* out_used, u32* out_total, u64* out_seq) {
    if (out_used) {
        u32 u = 0;
        for (u32 i = 0; i < MSG_POOL_SIZE; i++) if (g_pool[i].used) u++;
        *out_used = u;
    }
    if (out_total) *out_total = MSG_POOL_SIZE;
    if (out_seq)   *out_seq = (g_seq > 0) ? (g_seq - 1) : 0;
}

}  // namespace ipc
