// kernel/ipc/msg.cpp — B1.2: Index-Based Message-Passing, persistent
//
// Design: Payload lebt komplett im LLRB, kein RAM-Pool.
//   MSG-Record:  key=NS_MSG|seq<<8|ch,       a=0,           b=len
//   MSGP-Record: key=NS_MSGP|seq<<8|ch|ci,   a=bytes[0..7], b=bytes[8..15]
//
// Layout MSGP-key:
//   bits 63..56 = 0x0A (NS_MSGP)
//   bits 55..8  = seq
//   bits 7..4   = ch (0..15)
//   bits 3..0   = chunk_idx (0..15)
//
// Nach WAL-Replay sind beide Record-Typen im Index -> msg_recv
// rekonstruiert Payload aus MSGP-Chunks, ohne extra Pool.
#include "kernel/ipc/msg.h"
#include "kernel/idx/idx.h"

extern volatile u64 g_tick;

namespace ipc {

static u64 g_seq = 1;

static void put_payload_chunks(u64 seq, u8 channel, const u8* data, u32 len) {
    u32 ci = 0;
    for (u32 off = 0; off < len; off += 16, ci++) {
        u64 a = 0, b = 0;
        u32 n = len - off;
        if (n > 16) n = 16;
        for (u32 k = 0; k < n && k < 8; k++) a |= (u64)data[off + k] << (8 * k);
        for (u32 k = 8; k < n; k++)         b |= (u64)data[off + k] << (8 * (k - 8));
        u64 key = idx::NS_MSGP
                | ((seq & 0x0000FFFFFFFFFFFFULL) << 8)
                | ((u64)(channel & 0x0F) << 4)
                | (u64)(ci & 0x0F);
        idx::put(key, idx::Type::MSG, a, b);
    }
}

int msg_send(u8 channel, const u8* data, u32 len) {
    if (channel >= MSG_CHANNELS) return -22;
    if (len > MSG_MAX_LEN) return -22;
    if (len == 0) return 0;

    u64 seq = g_seq++;
    put_payload_chunks(seq, channel, data, len);
    u64 key = idx::NS_MSG
            | ((seq & 0x0000FFFFFFFFFFFFULL) << 8)
            | (u64)(channel & 0xFF);
    idx::put(key, idx::Type::MSG, 0, (u64)len);
    return 0;
}

struct MsgFindCtx {
    u8   ch;
    u64  best_key;
    u64  best_len;
};
static void msg_find_visit(idx::Rec* r, void* vp) {
    MsgFindCtx* c = (MsgFindCtx*)vp;
    if ((r->key & 0xFF00000000000000ULL) != idx::NS_MSG) return;
    u8 ch = (u8)(r->key & 0xFF);
    if (ch != c->ch) return;
    if (c->best_key == 0 || r->key < c->best_key) {
        c->best_key = r->key;
        c->best_len = r->b;
    }
}

struct PayloadCtx {
    u64 seq;
    u8  ch;
    u8* out;
    u32 cap;
    u32 total;
};
static void msg_payload_visit(idx::Rec* r, void* vp) {
    PayloadCtx* c = (PayloadCtx*)vp;
    if ((r->key & 0xFF00000000000000ULL) != idx::NS_MSGP) return;
    u64 rseq = (r->key >> 8) & 0x0000FFFFFFFFFFFFULL;
    if (rseq != c->seq) return;
    u8 rch = (u8)((r->key >> 4) & 0x0F);
    if (rch != c->ch) return;
    u32 ci = (u32)(r->key & 0x0F);
    u32 off = ci * 16;
    if (off >= c->cap) return;
    u8 chunk[16];
    for (u32 k = 0; k < 8; k++) chunk[k]     = (u8)((r->a >> (8*k)) & 0xFF);
    for (u32 k = 0; k < 8; k++) chunk[8 + k] = (u8)((r->b >> (8*k)) & 0xFF);
    for (u32 k = 0; k < 16 && (off + k) < c->cap && (off + k) < c->total; k++) {
        c->out[off + k] = chunk[k];
    }
    if (off + 16 > c->total) c->total = off + 16;
}

struct DelCtx { u64 seq; u8 ch; u64 keys[64]; u32 n; };
static void msg_del_visit(idx::Rec* r, void* vp) {
    DelCtx* c = (DelCtx*)vp;
    if (c->n >= 64) return;
    u64 hi = r->key & 0xFF00000000000000ULL;
    if (hi == idx::NS_MSG) {
        u64 rseq = (r->key >> 8) & 0x0000FFFFFFFFFFFFULL;
        u8 rch = (u8)(r->key & 0xFF);
        if (rseq == c->seq && rch == c->ch) c->keys[c->n++] = r->key;
    } else if (hi == idx::NS_MSGP) {
        u64 rseq = (r->key >> 8) & 0x0000FFFFFFFFFFFFULL;
        u8 rch = (u8)((r->key >> 4) & 0x0F);
        if (rseq == c->seq && rch == c->ch) c->keys[c->n++] = r->key;
    }
}

int msg_recv(u8 channel, u8* out, u32 cap, u32 timeout_ms) {
    if (channel >= MSG_CHANNELS) return -22;
    u64 deadline = timeout_ms ? (g_tick + (timeout_ms / 10) + 1) : 0;

    for (;;) {
        MsgFindCtx f = { channel, 0, 0 };
        idx::scan(idx::Type::MSG, msg_find_visit, &f);

        if (f.best_key != 0) {
            u64 seq = (f.best_key >> 8) & 0x0000FFFFFFFFFFFFULL;
            u32 total = (f.best_len > cap) ? cap : (u32)f.best_len;

            PayloadCtx pc = { seq, channel, out, cap, total };
            idx::scan(idx::Type::MSG, msg_payload_visit, &pc);
            u32 n = (pc.total < total) ? pc.total : total;

            DelCtx dc = { seq, channel, {0}, 0 };
            idx::scan(idx::Type::MSG, msg_del_visit, &dc);
            for (u32 i = 0; i < dc.n; i++) idx::remove(dc.keys[i]);

            return (int)n;
        }

        if (timeout_ms == 0) return -11;
        if (g_tick >= deadline) return -110;
        __asm__ volatile("sti; hlt");
    }
}

void msg_stats(u32* out_used, u32* out_total, u64* out_seq) {
    struct C { u32 n; } c = {0};
    idx::scan(idx::Type::MSG,
              [](idx::Rec* r, void* vp) {
                  if ((r->key & 0xFF00000000000000ULL) == idx::NS_MSG)
                      ((C*)vp)->n++;
              }, &c);
    if (out_used)  *out_used  = c.n;
    if (out_total) *out_total = MSG_POOL_SIZE;
    if (out_seq)   *out_seq   = (g_seq > 0) ? (g_seq - 1) : 0;
}

}  // namespace ipc
