// kernel/ipc/pipe.h — A9b Pipes
#ifndef KERNEL_IPC_PIPE_H
#define KERNEL_IPC_PIPE_H

#include "kernel/hal/hal.h"

namespace ipc {

static constexpr u32 PIPE_MAX = 16;
static constexpr u32 PIPE_BUF = 4096;

struct Pipe {
    u8   buf[PIPE_BUF];
    u32  rpos, wpos, count;
    u32  readers, writers;
    bool used;
};

// Erzeugt neue Pipe, gibt 0 bei Erfolg, -errno sonst.
// fds werden ueber Out-Parameter zurueckgegeben (Kernel-FDs, User macht Mapping).
i64  pipe_alloc(int* out_rfd, int* out_wfd);

// Pipe-I/O. Non-blocking: -EAGAIN wenn leer/voll.
i64  pipe_read(int idx, u8* dst, u64 len);
i64  pipe_write(int idx, const u8* src, u64 len);

// Schliesst Reader/Writer-Seite. Bei 0/0 wird die Pipe freigegeben.
void pipe_close(int idx, bool was_reader);

} // namespace ipc

#endif
