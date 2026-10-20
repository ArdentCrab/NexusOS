// kernel/syscall/syscall.cpp — Refactor Session 9
#include "kernel/syscall/syscall.h"
#include "kernel/ipc/ipc.h"
#include "kernel/syscall/per_cpu.h"
#include "kernel/task/task.h"
#include "kernel/task/fd.h"
#include "kernel/mm/mm.h"

namespace syscall_msr {
static constexpr u32 MSR_EFER   = 0xC0000080;
static constexpr u32 MSR_STAR   = 0xC0000081;
static constexpr u32 MSR_LSTAR  = 0xC0000082;
static constexpr u32 MSR_SFMASK = 0xC0000084;
static constexpr u32 MSR_GS_K   = 0xC0000101;   // IA32_GS_BASE
static constexpr u32 MSR_GS_U   = 0xC0000102;   // IA32_KERNEL_GS_BASE

void init() {
    u64 efer = hal::rdmsr(MSR_EFER);
    efer |= 1;                        // SCE (Syscall Enable)
    hal::wrmsr(MSR_EFER, efer);

    // STAR: [63:48]=user CS base, [47:32]=kernel CS base
    u64 star = ((u64)0x18 << 48) | ((u64)0x08 << 32);
    hal::wrmsr(MSR_STAR, star);

    hal::wrmsr(MSR_LSTAR,  (u64)&syscall_entry);
    hal::wrmsr(MSR_SFMASK, 0x700);    // clear IF | DF | TF bei syscall-entry

    // GS-Basen: kernel-mode GS = &g_cpu, user-mode GS = 0
    hal::wrmsr(MSR_GS_K, (u64)&g_cpu);
    hal::wrmsr(MSR_GS_U, 0);
}
} // namespace syscall_msr

extern "C" u64 syscall_dispatch(u64* regs) {
    i64 nr = (i64)regs[6];
    u64 a0 = regs[5];   // rdi
    u64 a1 = regs[4];   // rsi
    u64 a2 = regs[3];   // rdx

    if (nr == 50) {  // publish(key, type, a, b)
        ipc::publish(a0, (u8)a1, a2, regs[1]);
        return 0;
    }
    if (nr == 51) {  // subscribe(type_filter)
        task::TCB* t = task::current;
        if (!t) return (u64)(i64)-3;
        i64 h = ipc::sub_alloc(t->subs, a0);
        return (u64)h;
    }
    if (nr == 52) {  // wait(handle, out_ptr, timeout_ms)
        task::TCB* t = task::current;
        if (!t) return (u64)(i64)-3;
        if (!vmm::user_range_ok(a1, 16, true)) return (u64)(i64)-14;
        u64 k = 0, av = 0;
        i64 r = ipc::wait_event(t->subs, a0, a2, &k, &av);
        if (r < 0) return (u64)r;
        *(u64*)(a1 + 0) = k;
        *(u64*)(a1 + 8) = av;
        return 0;
    }
    if (nr == 53) {  // unsubscribe(handle)
        task::TCB* t = task::current;
        if (!t) return (u64)(i64)-3;
        return (u64)ipc::sub_unsubscribe(t->subs, a0);
    }
    if (nr == 0) {   // read(fd, buf, len)
        return (u64)sys_read(a0, a1, a2);
    }
    if (nr == 1) {   // write(fd, buf, len)
        if (a2 == 0) return 0;
        if (!vmm::user_range_ok(a1, a2, false)) return (u64)E_FAULT;

        // fd 1/2 = stdout/stderr -> Serial
        if (a0 == 1 || a0 == 2) {
            hal::serial_lock();
            hal::serial_write_bulk_raw((const char*)a1, a2);
            hal::serial_unlock();
            return a2;
        }

        // fd >= 3 = Datei -> in ram_buf schreiben, dirty markieren
        if (a0 < (u64)FD_MAX) {
            task::TCB* t = task::current;
            if (!t) return (u64)E_BADF;
            FileDesc* d = &t->fds[a0];
            if (!d->in_use) return (u64)E_BADF;
            if (!(d->flags & O_WRONLY)) return (u64)E_BADF;

            // NEXUS append-by-default. O_APPEND erzwingt Append (POSIX-Vorrang),
            // O_REWRITE schaltet Linux-Semantik ein (Offset-basiert).
            if (!(d->flags & O_REWRITE) || (d->flags & O_APPEND)) {
                d->offset = d->size;
            }

            u64 needed = d->offset + a2;
            if (needed > d->capacity) {
                // Puffer wachsen lassen (verdoppeln bis >= needed)
                u64 newcap = d->capacity ? d->capacity : 64;
                while (newcap < needed) newcap *= 2;
                if (newcap > 1024 * 1024) return (u64)E_NOSPC;
                char* np = (char*)heap::alloc((usize)newcap);
                if (!np) return (u64)E_NOSPC;
                if (d->ram_buf && d->size > 0) {
                    const char* old = (const char*)d->ram_buf;
                    for (u64 i = 0; i < d->size; i++) np[i] = old[i];
                }
                if (d->ram_buf) heap::free((void*)d->ram_buf);
                d->ram_buf = (u64)np;
                d->capacity = newcap;
            }

            u8* dst = (u8*)d->ram_buf + d->offset;
            const u8* src = (const u8*)a1;
            for (u64 i = 0; i < a2; i++) dst[i] = src[i];
            d->offset += a2;
            if (d->offset > d->size) d->size = d->offset;
            d->dirty = 1;
            return a2;
        }
        return (u64)E_BADF;
    }
    if (nr == 8) {   // lseek(fd, offset, whence)
        return (u64)sys_lseek(a0, (i64)a1, (u32)a2);
    }
    if (nr == 140) { // getpriority(pid)
        return (u64)task::sys_getpriority(a0);
    }
    if (nr == 141) { // setpriority(pid, prio)
        return (u64)task::sys_setpriority(a0, a1);
    }
    if (nr == 35) {  // sleep(ms)
        return (u64)task::sys_sleep(a0);
    }
    if (nr == 39) {  // getpid()
        task::TCB* t = task::current;
        return t ? t->pid : 0;
    }
    if (nr == 2) {   // open(name, flags)
        return (u64)sys_open(a0, a1);
    }
    if (nr == 3) {   // close(fd)
        return (u64)sys_close(a0);
    }
    if (nr == 24) {  // yield()  (sched_yield)
        // KNOWN LIMITATION (T4): yield ist derzeit no-op. Der Scheduler
        // wechselt erst beim naechsten PIT-Tick (bis zu 10 ms). Ein echtes
        // yield erfordert einen separaten Syscall-Exit-Pfad, der direkt
        // in den Scheduler springt (kein sysretq).
        return 0;
    }
    if (nr == 60) {  // exit(code)
        task::TCB* t = task::current;
        if (t) {
            hal::pf("[EXIT] pid=%u code=%u\n", (u32)t->pid, (u32)a0);
            t->dead = 1; task::mark_dead(t);
        }
        // KNOWN LIMITATION (S2): blockiert bis zu 10 ms bis zum naechsten
        // PIT-Tick. Bei kurzen Kind-Prozessen summiert sich das. Fix waere
        // ein direkter schedule()-Sprung aus dem Syscall-Exit-Stub heraus.
        // NICHT per sysretq zum User-Code zurueck: nach sys_exit hat der
        // User-Code keinen definierten Nachfolger (Compiler emittiert
        // hinter __builtin_unreachable() nichts). Stattdessen im Kernel
        // warten, bis der naechste Timer-IRQ den Scheduler triggert.
        __asm__ volatile("sti");
        for (;;) __asm__ volatile("hlt");
    }
    return (u64)E_NOSYS;
}

__asm__(
    ".global syscall_entry\n"
    "syscall_entry:\n"
    "  swapgs\n"                       // GS -> kernel per-CPU
    "  movq %rsp, %gs:56\n"            // save user RSP
    "  movq %gs:48, %rsp\n"            // load kernel RSP
    "  pushq $0\n"                     // 16-byte alignment padding
    "  pushq %rcx\n"                   // [8] user RIP
    "  pushq %r11\n"                   // [7] user RFLAGS
    "  pushq %rax\n"                   // [6] syscall nr
    "  pushq %rdi\n"                   // [5]
    "  pushq %rsi\n"                   // [4]
    "  pushq %rdx\n"                   // [3]
    "  pushq %r10\n"                   // [2]
    "  pushq %r8\n"                    // [1]
    "  pushq %r9\n"                    // [0]
    "  movq %rsp, %rdi\n"
    "  call syscall_dispatch\n"
    "  popq %r9\n"
    "  popq %r8\n"
    "  popq %r10\n"
    "  popq %rdx\n"
    "  popq %rsi\n"
    "  popq %rdi\n"
    "  addq $8, %rsp\n"                // discard saved nr (rax behaelt ret-value)
    "  popq %r11\n"
    "  popq %rcx\n"
    "  addq $8, %rsp\n"                // discard padding
    "  movq %gs:56, %rsp\n"            // restore user RSP
    "  swapgs\n"
    "  sysretq\n"
);

