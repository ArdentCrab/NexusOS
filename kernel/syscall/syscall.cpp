// kernel/syscall/syscall.cpp — Refactor Session 9
#include "kernel/syscall/syscall.h"
#include "kernel/ipc/ipc.h"
#include "kernel/ipc/pipe.h"
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

    // STAR: [63:48]=user CS base (SYSRET), [47:32]=kernel CS base (SYSCALL)
    //
    // NEXUS GDT: 0=null, 1=K(CS), 2=K(DS), 3=U(CS,0x1b), 4=U(DS,0x23), 5=TSS
    //
    // SYSCALL: CS = STAR[47:32]      = 0x08 (Kernel-Code)   OK
    //          SS = STAR[47:32] + 8  = 0x10 (Kernel-Data)   OK
    //
    // SYSRET:  CS = STAR[63:48] + 16 | RPL3
    //          SS = STAR[63:48] +  8 | RPL3
    //
    // B1-Fix: alter Wert 0x18 -> SYSRET CS = 0x18+0x10|3 = 0x2b (TSS-Selector!)
    //         User-Code lief mit CS=0x2b, beim naechsten Timer-IRQ push
    //         cs=0x2b in den Frame -> PANIC.
    //
    // Neuer Wert 0x08:
    //   SYSRET CS = 0x08+0x10|3 = 0x1b (User-Code)    OK
    //   SYSRET SS = 0x08+0x08|3 = 0x13 (Kernel-Data RPL3)
    //
    // SS=0x13 ist im 64-bit Mode funktional OK (SS wird fuer Limits
    // ignoriert, muss nur gueltig sein). Guard im irq_dispatch prueft
    // 0x13 mit.
    u64 star = ((u64)0x08 << 48) | ((u64)0x08 << 32);
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
            if (d->kind == 2) {
                // B1-TEMP: blocking disabled (pipe_write)
                return (u64)ipc::pipe_write((int)d->pipe_idx, (const u8*)a1, a2);
            }
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
    if (nr == 22) {  // pipe(fds[2]) — A9b
        task::TCB* t = task::current;
        if (!t) return (u64)(i64)-3;
        if (!vmm::user_range_ok(a0, 8, true)) return (u64)(i64)-14;
        int r_idx = -1, w_idx = -1;
        i64 r = ipc::pipe_alloc(&r_idx, &w_idx);
        if (r < 0) return (u64)r;
        int rfd = fd_alloc();
        int wfd = fd_alloc();
        if (rfd < 0 || wfd < 0) {
            if (rfd >= 0) fd_free(rfd);
            if (wfd >= 0) fd_free(wfd);
            ipc::pipe_close(r_idx, true);
            ipc::pipe_close(w_idx, false);
            return (u64)(i64)-23;
        }
        t->fds[rfd].kind = 1; t->fds[rfd].pipe_idx = (u16)r_idx;
        t->fds[wfd].kind = 2; t->fds[wfd].pipe_idx = (u16)w_idx;
        *(u64*)(a0 + 0) = (u64)rfd;
        *(u64*)(a0 + 8) = (u64)wfd;
        return 0;
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

// B1-FIX: syscall-Rueckweg nutzt iretq statt sysretq.
//
// Grund: SYSRET berechnet CS/SS aus STAR[63:48]:
//   CS = STAR[63:48] + 16 | RPL3
//   SS = STAR[63:48] +  8 | RPL3
// NEXUS-GDT: 3=U-CS(0x1b), 4=U-DS(0x23), 5=TSS(0x2b).
// Kein STAR-Wert liefert gleichzeitig CS=0x1b UND SS=0x23:
//   STAR=0x08 -> CS=0x1b, SS=0x13 (Kernel-Data! #GP)
//   STAR=0x18 -> CS=0x2b, SS=0x23 (TSS als CS! Guard-Panic)
//
// Loesung: iretq. CS/SS kommen aus dem Frame, unabhaengig von STAR.
// Wir bauen den iretq-Frame explizit mit CS=0x1b, SS=0x23.
//
// Stack-Layout nach dem Dispatch (von oben gepoppt):
//   [nr][RIP][CS][RFLAGS][RSP][SS]
// Nach den 6 popq + addq $8 zeigt %rsp auf [RIP].
//
// Alignment: 5 (iretq) + 7 (GPRs) = 12 Pushes = 96 B, % 16 == 0.
// Damit ist die Stack-Ausrichtung fuer syscall_dispatch korrekt.
//
// rcx/r11 werden vor dem iretq aus dem Frame restauriert — User-Code
// sieht wie bisher seinen eigenen RIP in rcx und RFLAGS in r11.
__asm__(
    ".global syscall_entry\n"
    "syscall_entry:\n"
    "  swapgs\n"                       // GS -> kernel per-CPU
    "  movq %rsp, %gs:56\n"            // save user RSP
    "  movq %gs:48, %rsp\n"            // load kernel RSP
    // --- iretq-Frame bauen (Reihenfolge: SS, RSP, RFLAGS, CS, RIP) ---
    "  pushq $0x23\n"                  // SS  (U-Data, RPL3)
    "  pushq %gs:56\n"                 // user RSP
    "  pushq %r11\n"                   // user RFLAGS
    "  pushq $0x1b\n"                  // CS  (U-Code, RPL3)
    "  pushq %rcx\n"                   // user RIP
    // --- GPRs fuer dispatch (Layout identisch zum alten Stub) ---
    "  pushq %rax\n"                   // regs[6] = syscall nr
    "  pushq %rdi\n"                   // regs[5]
    "  pushq %rsi\n"                   // regs[4]
    "  pushq %rdx\n"                   // regs[3]
    "  pushq %r10\n"                   // regs[2]
    "  pushq %r8\n"                    // regs[1]
    "  pushq %r9\n"                    // regs[0]
    "  movq %rsp, %rdi\n"
    "  call syscall_dispatch\n"
    "  popq %r9\n"
    "  popq %r8\n"
    "  popq %r10\n"
    "  popq %rdx\n"
    "  popq %rsi\n"
    "  popq %rdi\n"
    "  addq $8, %rsp\n"                // discard saved nr (%rax = ret-value)
    // --- rcx/r11 aus Frame restaurieren (User-Kompatibilitaet) ---
    "  movq 0(%rsp), %rcx\n"           // rcx = user RIP
    "  movq 16(%rsp), %r11\n"          // r11 = user RFLAGS
    "  swapgs\n"
    "  iretq\n"
);

