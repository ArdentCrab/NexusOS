// kernel/task/fd.cpp — Refactor Session 8
#include "kernel/task/fd.h"
#include "kernel/idx/idx.h"
#include "kernel/task/task.h"
#include "kernel/fs/fs.h"
#include "kernel/mm/mm.h"

int fd_alloc() {
    task::TCB* t = task::current;
    if (!t) return -1;
    for (int fd = 3; fd < FD_MAX; fd++) {
        if (!t->fds[fd].in_use) {
            t->fds[fd].in_use = 1;
            t->fds[fd].key = 0;
            t->fds[fd].offset = 0;
            t->fds[fd].size = 0;
            t->fds[fd].ram_buf = 0;
            t->fds[fd].flags = 0;
            return fd;
        }
    }
    return -1;
}

// fd-Slot freigeben. Puffer wird freigegeben, FILE-Record bleibt.
int fd_free(int fd) {
    task::TCB* t = task::current;
    if (!t) return -1;
    if (fd < 3 || fd >= FD_MAX) return -1;
    if (!t->fds[fd].in_use) return -1;
    if (t->fds[fd].ram_buf) {
        heap::free((void*)t->fds[fd].ram_buf);
        t->fds[fd].ram_buf = 0;
    }
    t->fds[fd].in_use = 0;
    t->fds[fd].dirty = 0;
    t->fds[fd].capacity = 0;
    return 0;
}

// Flush: schreibt den fd-Puffer in den FILE-Record, wenn dirty.
// Rueckgabe 0 = OK oder nichts zu tun, <0 = Fehler.
int fd_flush(int fd) {
    task::TCB* t = task::current;
    if (!t) return -1;
    if (fd < 3 || fd >= FD_MAX) return -1;
    if (!t->fds[fd].in_use) return -1;
    if (!t->fds[fd].dirty) return 0;
    if (!t->fds[fd].ram_buf || t->fds[fd].size == 0) {
        t->fds[fd].dirty = 0;
        return 0;
    }
    int r = fs::write_file(t->fds[fd].name,
                           (const char*)t->fds[fd].ram_buf,
                           (usize)t->fds[fd].size);
    if (r < 0) return r;
    t->fds[fd].dirty = 0;
    return 0;
}

// Kopiert bis zu max-1 Bytes eines User-Strings in einen Kernel-Puffer.
// Prueft jede beruehrte Page einzeln, weil der String am Rand der letzten
// Stack-Page liegen kann (dann ist [uva, uva+max) nicht komplett gemappt,
// obwohl der String selbst in die Page passt).
// Rueckgabe: Laenge ohne Null, oder -errno.
static i64 copy_user_str(u64 uva, char* dst, u64 max) {
    if (max == 0) return (i64)E_INVAL;
    const char* src = (const char*)uva;
    u64 i = 0;
    while (i + 1 < max) {
        u64 cur = uva + i;
        if (!vmm::user_range_ok(cur & ~0xFFFULL, 1, false))
            return (i64)E_FAULT;
        dst[i] = src[i];
        if (src[i] == 0) return (i64)i;
        i++;
    }
    dst[max - 1] = 0;
    return (i64)E_INVAL;
}

// Kernel-interne Variante von sys_open: nimmt einen Kernel-String.
// Wird von cmd_fdtest (Kernel-Kontext-Test) und vom Syscall-Wrapper
// (mit vorher kopiertem Namen) benutzt.
i64 sys_open_kernel(const char* name, u64 flags) {
    if (!name || !*name) return (i64)E_INVAL;
    usize nlen = 0; while (name[nlen] && nlen < 31) nlen++;

    u64 h = fs::hash_name(name);
    u64 fkey = idx::NS_FILE | h;
    idx::Rec* r = idx::find(fkey);

    bool exists = (r && r->type == (u32)idx::Type::FILE);
    if (!exists && !(flags & O_CREAT)) return (i64)E_NOENT;

    // O_TRUNC: existierende Datei sofort auf 0 leeren (POSIX-Verhalten).
    u64 size;
    if (exists && (flags & O_TRUNC)) {
        // Datei auf 0 Bytes setzen, alter Inhalt bleibt im FS bis zum close
        // (dirty=1 in fd_slot, flush schreibt leere Datei).
        size = 0;
    } else {
        size = exists ? r->a : 0;
    }
    u64 ram_buf = 0;
    if (size > 0) {
        char* p = (char*)heap::alloc((usize)size + 1);
        if (!p) return (i64)E_NOSPC;
        int got = fs::cat_file(name, p, (usize)size + 1);
        if (got < 0) { heap::free(p); return (i64)E_INVAL; }
        ram_buf = (u64)p;
    }

    int fd = fd_alloc();
    if (fd < 0) {
        if (ram_buf) heap::free((void*)ram_buf);
        return (i64)E_NOSPC;
    }
    task::TCB* t = task::current;
    t->fds[fd].key      = fkey;
    t->fds[fd].offset   = 0;
    t->fds[fd].size     = size;
    t->fds[fd].capacity = size;
    t->fds[fd].ram_buf  = ram_buf;
    t->fds[fd].flags    = (u32)flags;
    t->fds[fd].dirty    = 0;
    for (usize i = 0; i < nlen; i++) t->fds[fd].name[i] = name[i];
    t->fds[fd].name[nlen] = 0;
    return (i64)fd;
}

// Syscall-Einstiegspunkt: kopiert den User-String und ruft die
// Kernel-Variante.
i64 sys_open(u64 u_name, u64 flags) {
    char name[40];
    i64 n = copy_user_str(u_name, name, sizeof(name));
    if (n < 0) return n;
    if (n == 0) return (i64)E_INVAL;
    return sys_open_kernel(name, flags);
}

// sys_close(fd)
i64 sys_close(u64 fd) {
    if (fd < 3 || fd >= FD_MAX) return (i64)E_BADF;
    task::TCB* t = task::current;
    if (!t || !t->fds[fd].in_use) return (i64)E_BADF;
    int fr = fd_flush((int)fd);
    if (fr < 0) return (i64)fr;   // Flush-Fehler nicht schlucken
    return (i64)fd_free((int)fd);
}

// Kernel-interne Variante von sys_read: nimmt einen Kernel-Puffer,
// ueberspringt user_range_ok. Wird vom Syscall-Wrapper (nach Pruefung)
// und vom Kernel-Kontext-Test benutzt.
i64 sys_read_kernel(int fd, void* kbuf, u64 len) {
    if (fd < 3 || fd >= FD_MAX) return (i64)E_BADF;
    if (len == 0) return 0;
    task::TCB* t = task::current;
    if (!t) return (i64)E_BADF;
    FileDesc* d = &t->fds[fd];
    if (!d->in_use) return (i64)E_BADF;
    if (d->offset >= d->size) return 0;   // EOF

    u64 avail = d->size - d->offset;
    u64 n = (len < avail) ? len : avail;
    if (n == 0 || !d->ram_buf) return 0;

    const u8* src = (const u8*)d->ram_buf + d->offset;
    u8* dst = (u8*)kbuf;
    for (u64 i = 0; i < n; i++) dst[i] = src[i];
    d->offset += n;
    return (i64)n;
}

// Syscall-Einstiegspunkt: prueft User-Pointer, ruft die Kernel-Variante.
i64 sys_read(u64 fd, u64 buf, u64 len) {
    if (fd < 3 || fd >= FD_MAX) return (i64)E_BADF;
    if (len == 0) return 0;
    if (!vmm::user_range_ok(buf, len, true)) return (i64)E_FAULT;
    return sys_read_kernel((int)fd, (void*)buf, len);
}

