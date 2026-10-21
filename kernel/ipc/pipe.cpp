// kernel/ipc/pipe.cpp — A9b Pipes
#include "kernel/ipc/pipe.h"

namespace ipc {

static Pipe g_pipes[PIPE_MAX];

i64 pipe_alloc(int* out_rfd, int* out_wfd) {
    for (u32 i = 0; i < PIPE_MAX; i++) {
        if (!g_pipes[i].used) {
            Pipe* p = &g_pipes[i];
            for (u32 k = 0; k < PIPE_BUF; k++) p->buf[k] = 0;
            p->rpos = 0; p->wpos = 0; p->count = 0;
            p->readers = 1; p->writers = 1; p->used = true;
            *out_rfd = (int)i;
            *out_wfd = (int)i;
            return 0;
        }
    }
    return -23;  // ENFILE
}

i64 pipe_read(int idx, u8* dst, u64 len) {
    if (idx < 0 || idx >= (int)PIPE_MAX) return -22;
    Pipe* p = &g_pipes[idx];
    if (!p->used) return -22;
    if (len == 0) return 0;
    if (p->count == 0) {
        if (p->writers == 0) return 0;   // EOF: alle Writer zu
        return -11;                       // EAGAIN: leer, aber Writer leben
    }
    u64 n = len < p->count ? len : p->count;
    for (u64 i = 0; i < n; i++) {
        dst[i] = p->buf[p->rpos];
        p->rpos = (p->rpos + 1) % PIPE_BUF;
        p->count--;
    }
    return (i64)n;
}

i64 pipe_write(int idx, const u8* src, u64 len) {
    if (idx < 0 || idx >= (int)PIPE_MAX) return -22;
    Pipe* p = &g_pipes[idx];
    if (!p->used) return -22;
    if (len == 0) return 0;
    if (p->readers == 0) return -32;   // EPIPE: alle Reader zu
    u64 space = PIPE_BUF - p->count;
    if (space == 0) return -11;  // EAGAIN
    u64 n = len < space ? len : space;
    for (u64 i = 0; i < n; i++) {
        p->buf[p->wpos] = src[i];
        p->wpos = (p->wpos + 1) % PIPE_BUF;
        p->count++;
    }
    return (i64)n;
}

void pipe_close(int idx, bool was_reader) {
    if (idx < 0 || idx >= (int)PIPE_MAX) return;
    Pipe* p = &g_pipes[idx];
    if (!p->used) return;
    if (was_reader) { if (p->readers > 0) p->readers--; }
    else            { if (p->writers > 0) p->writers--; }
    if (p->readers == 0 && p->writers == 0) {
        p->used = false;
        p->count = 0;
    }
}

} // namespace ipc
