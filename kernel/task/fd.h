// kernel/task/fd.h — Refactor Session 8
#ifndef KERNEL_TASK_FD_H
#define KERNEL_TASK_FD_H

#include "kernel/hal/hal.h"

struct FileDesc {
    u64  key;        // idx-Key des FILE-Records
    u64  offset;     // Lese/Schreib-Position
    u64  size;       // Dateigroesse in Bytes
    u64  capacity;   // Allokierte Groesse des ram_buf (>= size)
    u64  ram_buf;    // Kernel-Heap-Puffer (0 = leer)
    u32  flags;      // O_RDONLY / O_WRONLY / O_RDWR / O_CREAT
    u32  in_use;     // 0 = frei, 1 = belegt
    u32  dirty;      // 1 = Puffer weicht von der Datei ab -> Flush bei close
    u16  kind;        // 0=file, 1=pipe-read, 2=pipe-write
    u16  pipe_idx;    // 0..15
    char name[32];   // Dateiname fuer Flush bei close
};
enum : u32 {
    O_RDONLY  = 1,
    O_WRONLY  = 2,
    O_RDWR    = 3,
    O_CREAT   = 4,
    O_TRUNC   = 8,
    O_APPEND  = 16,   // POSIX; Append ist bereits Default
    O_REWRITE = 32,   // NEXUS-Extension: Linux-Semantik
};
static constexpr int FD_MAX   = 32;
static constexpr int FD_STDIN = 0;
static constexpr int FD_STDOUT= 1;
static constexpr int FD_STDERR= 2;

// Linux-kompatible Errno-Werte.
// Rueckgabe >= 0 = Erfolg, < 0 (als signed interpretiert) = -errno.
enum : i64 {
    E_PERM   = -1,
    E_NOENT  = -2,
    E_BADF   = -9,
    E_FAULT  = -14,
    E_INVAL  = -22,
    E_NOSPC  = -28,
    E_NOSYS  = -38,
};

int  fd_alloc();
int  fd_free(int fd);
int  fd_flush(int fd);
i64  sys_open_kernel(const char* name, u64 flags);
i64  sys_read_kernel(int fd, void* kbuf, u64 len);
i64  sys_open(u64 u_name, u64 flags);
i64  sys_close(u64 fd);
i64  sys_read(u64 fd, u64 buf, u64 len);

enum : u32 {
    SEEK_SET = 0,
    SEEK_CUR = 1,
    SEEK_END = 2,
};

i64  sys_lseek(u64 fd, i64 offset, u32 whence);

#endif
