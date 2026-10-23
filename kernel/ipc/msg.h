#pragma once
#include "kernel/hal/hal.h"

namespace ipc {

static constexpr u32 MSG_POOL_SIZE = 64;
static constexpr u32 MSG_MAX_LEN   = 255;
static constexpr u32 MSG_CHANNELS  = 16;

// Sendet Nachricht an Channel (0..15). In-LLRB-Record: key=NS_MSG|seq<<8|ch,
// a=pool_slot, b=len. Rueckgabe: 0=OK, <0=Fehler.
int  msg_send(u8 channel, const u8* data, u32 len);

// Empfaengt aelteste Nachricht auf Channel. timeout_ms=0: non-blocking.
// Rueckgabe: Bytes gelesen (>=0), -11=EAGAIN, -110=ETIMEDOUT, sonst Fehler.
int  msg_recv(u8 channel, u8* out, u32 cap, u32 timeout_ms);

// Stats: benutzte Pool-Slots, Gesamt-Pool, letzte Seq-Nummer.
void msg_stats(u32* out_used, u32* out_total, u64* out_seq);

}  // namespace ipc
