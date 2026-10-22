// ============================================================================
//  NEXUS-OS v4.0 :: Slab + Keyboard + Event-Refactor + Idle-Fix
//  x86_64 | Limine 8.x | C++23 | -ffreestanding
// ============================================================================
#include "kernel/hal/hal.h"
#include "kernel/trig/trig.h"
#include "kernel/editor/editor.h"
#include "kernel/shell/shell.h"
#include "kernel/main/main.h"
#include "kernel/mm/mm.h"
#include "kernel/idx/idx.h"
#include "kernel/idx/llrb_test.h"
#include "kernel/task/fd.h"
#include "kernel/task/task.h"
#include "kernel/syscall/per_cpu.h"
#include "kernel/syscall/syscall.h"
#include "kernel/elf64/elf64.h"

// mm-Symbole als mem::-Mitglieder; Aufrufe bleiben unqualifiziert.
using namespace mem;

// ---------------------------------------------------------------------------
// LIMINE
// ---------------------------------------------------------------------------
#define LIMREQ __attribute__((used, section(".limine_requests"))) static volatile
#define LIMSTART __attribute__((used, section(".limine_requests_start"))) \
    static volatile u64 _ls[4] = {0xf6b8f4b39de7d1ae,0xfab91a6940fcb9cf,0x785c6ed015d3e316,0x181e920a7852b9d9}
#define LIMEND __attribute__((used, section(".limine_requests_end"))) \
    static volatile u64 _le[2] = {0xadc0e0531bb10d03,0x9572709f31764c62}
#define LIMREV __attribute__((used, section(".limine_requests"))) \
    static volatile u64 _lr[3] = {0xf9562b2d5c95a6c8,0x6a7b384944536bdc,3}

struct mm_entry { u64 base, length, type; };
struct mm_resp  { u64 rev, count; mm_entry** entries; };
struct hhdm_resp{ u64 rev, offset; };
struct kaddr_resp{ u64 rev, phys, virt; };
enum : u64 { MM_USABLE = 0 };

LIMREQ struct { u64 id[4]; u64 rev; mm_resp*    response; } mm_req    = {
    {0xc7b1dd30df4c8b88,0x0a82e883a194f07b,0x67cf3d9d378a806f,0xe304acdfc50c3c62}, 0, nullptr};
LIMREQ struct { u64 id[4]; u64 rev; hhdm_resp*  response; } hhdm_req  = {
    {0xc7b1dd30df4c8b88,0x0a82e883a194f07b,0x48dcf1cb8ad2b852,0x63984e959a98244b}, 0, nullptr};
LIMREQ struct { u64 id[4]; u64 rev; kaddr_resp* response; } kaddr_req = {
    {0xc7b1dd30df4c8b88,0x0a82e883a194f07b,0x71ba76863cc55f63,0xb2644a48c516a487}, 0, nullptr};
LIMSTART; LIMEND; LIMREV;

// ---------------------------------------------------------------------------
// HAL
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// GDT
// ---------------------------------------------------------------------------
struct __attribute__((packed)) Tss64 {
    u32 reserved0;
    u64 rsp0;
    u64 rsp1;
    u64 rsp2;
    u64 reserved1;
    u64 ist[7];
    u64 reserved2;
    u16 reserved3;
    u16 iomap_base;
};
static_assert(sizeof(Tss64) == 104, "TSS muss 104 Bytes sein");

namespace gdt {
struct __attribute__((packed)) E { u16 l,h1; u8 m,a,g,h2; };
struct __attribute__((packed)) P { u16 limit; u64 base; };
static E ent[7];       // 0=null 1=kcode 2=kdata 3=ucode 4=udata 5=tss_lo 6=tss_hi
static P p;
static Tss64 tss;

extern "C" void gdt_load(P*);
extern "C" void tss_load(u16 sel);
__asm__(
    ".global gdt_load\ngdt_load:\n"
    "  lgdt (%rdi)\n"
    "  mov $0x10,%ax\n"
    "  mov %ax,%ds\n  mov %ax,%es\n  mov %ax,%ss\n  mov %ax,%fs\n  mov %ax,%gs\n"
    "  pushq $0x08\n  leaq 1f(%rip),%rax\n  pushq %rax\n  lretq\n1: ret\n"
    ".global tss_load\ntss_load:\n"
    "  ltr %di\n  ret\n");

static void set_flat(int i, u8 access, u8 flags) {
    ent[i].l=0; ent[i].h1=0; ent[i].m=0; ent[i].a=access; ent[i].g=flags; ent[i].h2=0;
}
static void set_tss_desc(int i, u64 base, u32 limit) {
    u8* d = (u8*)&ent[i];
    for (int k = 0; k < 16; k++) d[k] = 0;
    d[0]  = (u8)(limit & 0xFF);
    d[1]  = (u8)((limit >> 8) & 0xFF);
    d[2]  = (u8)(base & 0xFF);
    d[3]  = (u8)((base >> 8) & 0xFF);
    d[4]  = (u8)((base >> 16) & 0xFF);
    d[5]  = 0x89;                          // present, DPL=0, type=9 (64-bit TSS)
    d[6]  = (u8)((limit >> 16) & 0x0F);
    d[7]  = (u8)((base >> 24) & 0xFF);
    u32 hi = (u32)(base >> 32);
    d[8]  = (u8)(hi & 0xFF);
    d[9]  = (u8)((hi >> 8) & 0xFF);
    d[10] = (u8)((hi >> 16) & 0xFF);
    d[11] = (u8)((hi >> 24) & 0xFF);
}

void init(){
    for (int k = 0; k < (int)sizeof(tss); k++) ((u8*)&tss)[k] = 0;
    tss.iomap_base = sizeof(Tss64);

    set_flat(0, 0x00, 0x00);   // null
    set_flat(1, 0x9A, 0x20);   // kcode  sel=0x08
    set_flat(2, 0x92, 0x00);   // kdata  sel=0x10
    set_flat(3, 0xFA, 0x20);   // ucode  sel=0x1B (RPL=3), long mode
    set_flat(4, 0xF2, 0x00);   // udata  sel=0x23 (RPL=3)
    set_tss_desc(5, (u64)&tss, (u32)sizeof(Tss64) - 1);

    p.limit = (u16)(sizeof(ent) - 1);
    p.base  = (u64)&ent;
    gdt_load(&p);
    tss_load(0x28);            // TR = TSS-Selektor
}

// Wird beim Aufbau des Ring-3-Einstiegs genutzt (Patch 2).
void tss_set_rsp0(u64 rsp0) { tss.rsp0 = rsp0; }
u64  tss_get_rsp0()         { return tss.rsp0; }
}

// ---------------------------------------------------------------------------
// IDT
// ---------------------------------------------------------------------------
struct Regs {
    u64 r15,r14,r13,r12,r11,r10,r9,r8;
    u64 rbp,rdi,rsi,rdx,rcx,rbx,rax;
    u64 vec, err, rip, cs, rflags;
};

namespace idt {
struct __attribute__((packed)) E { u16 o1,s; u8 ist,t; u16 o2; u32 o3; u32 z; };
struct __attribute__((packed)) P { u16 limit; u64 base; };
static E ent[256]; static P p;
extern "C" void idt_load(P*);
__asm__(".global idt_load\nidt_load:\n  lidt (%rdi)\n  ret\n");
void set(int i,u64 h,u8 t){
    ent[i].o1=(u16)h; ent[i].s=0x08; ent[i].ist=0; ent[i].t=t;
    ent[i].o2=(u16)(h>>16); ent[i].o3=(u32)(h>>32); ent[i].z=0;
}
extern "C" {
    extern u64 isr_addrs[32];
    extern u64 irq_addrs[16];
}
void init(){
    p.limit=sizeof(ent)-1; p.base=(u64)&ent;
    for(int i=0;i<32;i++) set(i, isr_addrs[i], 0x8E);
    for(int i=0;i<16;i++) set(32+i, irq_addrs[i], 0x8E);
    set(3, isr_addrs[3], 0xEE);   // #BP: DPL=3 fuer int3 aus Ring 3
    idt_load(&p);
}
}

static const char* exc_name(u64 v){
    static const char* names[] = {
        "DE","DB","NMI","BP","OF","BR","UD","NM","DF","CSO","TS","NP","SS","GP","PF","RES",
        "MF","AC","MC","XM","VE","CP","--","--","--","--","--","--","HV","VC","SX","--"};
    return v < 32 ? names[v] : "??";
}

extern "C" {
    extern volatile u64 g_irq_next_rsp;
    extern volatile u32 g_irq_next_mode;
}

extern "C" void exception_dispatch(Regs* r){
    static volatile u32 g_exc_depth = 0;
    if (g_exc_depth >= 3) {
        for (;;) __asm__ volatile("cli; hlt");
    }
    g_exc_depth = g_exc_depth + 1;
    __asm__ volatile("cli");
    u64 cr2 = hal::read_cr2();
    hal::pf("\n*** EXCEPTION #%u (%s) ***\n", (u32)r->vec, exc_name(r->vec));
    hal::pf("  rip=%p  cs=0x%x  rflags=%p\n", r->rip, (u32)r->cs, r->rflags);
    hal::pf("  err=0x%x", (u32)r->err);
    if (r->vec == 14) hal::pf("  cr2=%p", cr2);
    hal::pc('\n');
    hal::pf("  rax=%p rbx=%p rcx=%p rdx=%p\n", r->rax, r->rbx, r->rcx, r->rdx);

    // ===== A12-Diagnose (nur bei Ausnahmen, keine Performance) =====
    {
        u64 gs_msr = hal::rdmsr(0xC0000101);
        hal::pf("  GS_BASE MSR=%p\n", gs_msr);
        if (gs_msr != 0) {
            u64 gs_base = 0;
            __asm__ volatile("movq %%gs:0, %0" : "=r"(gs_base));
            hal::pf("  gs:0=%p  gs-valid=%u\n",
                    gs_base, gs_base ? 1u : 0u);
        } else {
            hal::ps("  gs:0 SKIPPED (GS_BASE=0)\n");
        }
    }
    hal::pf("  tss.rsp0=%p\n", gdt::tss_get_rsp0());
    hal::pf("  next_rsp=%p  next_mode=%u\n",
            (u64)g_irq_next_rsp, (u32)g_irq_next_mode);
    // IRETQ-Frame-Diagnose
    if (g_irq_next_rsp) {
        u64* fr = (u64*)g_irq_next_rsp;
        hal::pf("  fr[15..21] = vec=%p err=%p rip=%p cs=%p rflags=%p rsp=%p ss=%p\n",
                fr[15], fr[16], fr[17], fr[18], fr[19], fr[20], fr[21]);
        hal::pf("  expected: cs=0x1b/0x8, ss=0x23 if cs=0x1b\n");
    }
    task::dump_current();
    {
        u64 sp_val = 0;
        __asm__ volatile("movq %%rsp, %0" : "=r"(sp_val));
        u64* sp = (u64*)sp_val;
        hal::ps("  stack (8 qwords):\n");
        for (int i = 0; i < 8; i++) {
            hal::pf("    [%p] %p\n", (u64)(sp + i), sp[i]);
        }
    }
    // ===== Ende A12-Diagnose =====

    for (;;) __asm__ volatile("cli; hlt");
}

__asm__(
    ".section .text\n"
    ".macro ISR n\n.global isr_\\n\nisr_\\n:\n  pushq $0\n  pushq $\\n\n  jmp isr_c\n.endm\n"
    ".macro ISRE n\n.global isr_\\n\nisr_\\n:\n  pushq $\\n\n  jmp isr_c\n.endm\n"
    ".macro IRQ n\n.global irq_\\n\nirq_\\n:\n  pushq $0\n  pushq $(32+\\n)\n  jmp irq_c\n.endm\n"

    "ISR 0\nISR 1\nISR 2\nISR 3\nISR 4\nISR 5\nISR 6\nISR 7\n"
    "ISRE 8\nISR 9\nISRE 10\nISRE 11\nISRE 12\nISRE 13\nISRE 14\nISR 15\n"
    "ISR 16\nISRE 17\nISR 18\nISR 19\nISR 20\nISRE 21\nISR 22\nISR 23\n"
    "ISR 24\nISR 25\nISR 26\nISR 27\nISR 28\nISR 29\nISRE 30\nISR 31\n"

    "IRQ 0\nIRQ 1\nIRQ 2\nIRQ 3\nIRQ 4\nIRQ 5\nIRQ 6\nIRQ 7\n"
    "IRQ 8\nIRQ 9\nIRQ 10\nIRQ 11\nIRQ 12\nIRQ 13\nIRQ 14\nIRQ 15\n"

    "isr_c:\n"
    "  pushq %rax\n  pushq %rbx\n  pushq %rcx\n  pushq %rdx\n  pushq %rsi\n  pushq %rdi\n  pushq %rbp\n"
    "  pushq %r8\n  pushq %r9\n  pushq %r10\n  pushq %r11\n  pushq %r12\n  pushq %r13\n  pushq %r14\n  pushq %r15\n"
    "  movq %rsp,%rdi\n  call exception_dispatch\n  ud2\n"

    "irq_c:\n"
    "  testb $3, 24(%rsp)\n"
    "  jz 7f\n"
    "  swapgs\n"
    "7:\n"
    "  pushq %rax\n  pushq %rbx\n  pushq %rcx\n  pushq %rdx\n  pushq %rsi\n  pushq %rdi\n  pushq %rbp\n"
    "  pushq %r8\n  pushq %r9\n  pushq %r10\n  pushq %r11\n  pushq %r12\n  pushq %r13\n  pushq %r14\n  pushq %r15\n"
    "  movq %rsp,%rdi\n  call irq_dispatch\n"
    "  movq g_irq_next_rsp(%rip), %rdi\n"
    "  testq %rdi, %rdi\n"
    "  jz 1f\n"
    "  movq %rdi, %rsp\n"
    "  movl g_irq_next_mode(%rip), %ecx\n"
    "  testl %ecx, %ecx\n"
    "  jz 3f\n"
    "  popq %r15\n  popq %r14\n  popq %r13\n  popq %r12\n  popq %r11\n  popq %r10\n  popq %r9\n  popq %r8\n"
    "  popq %rbp\n  popq %rdi\n  popq %rsi\n  popq %rdx\n  popq %rcx\n  popq %rbx\n  popq %rax\n"
    "  addq $16,%rsp\n"
    "  testb $3, 8(%rsp)\n"
    "  jz 8f\n"
    "  swapgs\n"
    "8:\n"
    "  iretq\n"
    "3:\n"
    "  popq %r15\n  popq %r14\n  popq %r13\n  popq %r12\n  popq %rbp\n  popq %rbx\n  ret\n"
    "1:\n"
    "  movq %rax, %rsp\n"
    "  popq %r15\n  popq %r14\n  popq %r13\n  popq %r12\n  popq %r11\n  popq %r10\n  popq %r9\n  popq %r8\n"
    "  popq %rbp\n  popq %rdi\n  popq %rsi\n  popq %rdx\n  popq %rcx\n  popq %rbx\n  popq %rax\n"
    "  addq $16,%rsp\n"
    "  testb $3, 8(%rsp)\n"
    "  jz 9f\n"
    "  swapgs\n"
    "9:\n"
    "  iretq\n"
);

extern "C" {
    void isr_0();  void isr_1();  void isr_2();  void isr_3();
    void isr_4();  void isr_5();  void isr_6();  void isr_7();
    void isr_8();  void isr_9();  void isr_10(); void isr_11();
    void isr_12(); void isr_13(); void isr_14(); void isr_15();
    void isr_16(); void isr_17(); void isr_18(); void isr_19();
    void isr_20(); void isr_21(); void isr_22(); void isr_23();
    void isr_24(); void isr_25(); void isr_26(); void isr_27();
    void isr_28(); void isr_29(); void isr_30(); void isr_31();
    void irq_0();  void irq_1();  void irq_2();  void irq_3();
    void irq_4();  void irq_5();  void irq_6();  void irq_7();
    void irq_8();  void irq_9();  void irq_10(); void irq_11();
    void irq_12(); void irq_13(); void irq_14(); void irq_15();

    u64 isr_addrs[32] = {
        (u64)&isr_0,(u64)&isr_1,(u64)&isr_2,(u64)&isr_3,(u64)&isr_4,(u64)&isr_5,(u64)&isr_6,(u64)&isr_7,
        (u64)&isr_8,(u64)&isr_9,(u64)&isr_10,(u64)&isr_11,(u64)&isr_12,(u64)&isr_13,(u64)&isr_14,(u64)&isr_15,
        (u64)&isr_16,(u64)&isr_17,(u64)&isr_18,(u64)&isr_19,(u64)&isr_20,(u64)&isr_21,(u64)&isr_22,(u64)&isr_23,
        (u64)&isr_24,(u64)&isr_25,(u64)&isr_26,(u64)&isr_27,(u64)&isr_28,(u64)&isr_29,(u64)&isr_30,(u64)&isr_31,
    };
    u64 irq_addrs[16] = {
        (u64)&irq_0,(u64)&irq_1,(u64)&irq_2,(u64)&irq_3,(u64)&irq_4,(u64)&irq_5,(u64)&irq_6,(u64)&irq_7,
        (u64)&irq_8,(u64)&irq_9,(u64)&irq_10,(u64)&irq_11,(u64)&irq_12,(u64)&irq_13,(u64)&irq_14,(u64)&irq_15,
    };
}

extern "C" {
    volatile u64 g_irq_next_rsp  = 0;
    volatile u32 g_irq_next_mode = 0;
}


// mm (pmm, slab, heap, vmm, operator new/delete) moved to kernel/mm/

// ---------------------------------------------------------------------------
// ELF64-Loader (statisch, ET_EXEC, x86_64, no PIE)
// ---------------------------------------------------------------------------
// elf64 moved to kernel/elf64/

// ---------------------------------------------------------------------------
// WAL-Hooks (definiert in persist, aber idx braucht sie)
// ---------------------------------------------------------------------------
#include "kernel/idx/wal_hooks.h"
#include "kernel/fs/fs.h"
#include "kernel/persist/persist.h"
#include "kernel/dev/pci.h"
#include "kernel/dev/virtio_blk.h"
#include "kernel/dev/blockcache.h"
#include "kernel/dev/kbd.h"

// ---------------------------------------------------------------------------
// LLRB Index — mit Freigabe nach Delete
// ---------------------------------------------------------------------------
// idx (LLRB) + llrb_test moved to kernel/idx/


// ---------------------------------------------------------------------------
// FILE-DESCRIPTOR-TABLE
// ---------------------------------------------------------------------------
// Ein fd ist ein Index 0..31 in TCB.fds. Slots 0/1/2 sind reserviert
// (stdin/stdout/stderr). User-fds starten bei 3.
//
// Alles-in-RAM-Modell: sys_open laedt den ganzen Dateiinhalt in einen
// Kernel-Heap-Puffer (kein Extent-aware read_at). Das reicht fuer
// kleine Dateien (<1 MiB) und ist die Grundlage fuer A6c (fd-basiertes
// sys_write).
//
// Hinweis: Der Heap-Puffer waechst mit der Dateigroesse. Siehe
// KNOWN LIMITATION (P2) bei fs::cat_file fuer Details und Fix-Plan.
// FileDesc + O_* + FD_MAX moved to kernel/task/fd.h


// ---------------------------------------------------------------------------
// TASKS
// ---------------------------------------------------------------------------
// namespace task moved to kernel/task/task.cpp

// task_entry_inner moved to kernel/task/task.cpp

// ---------------------------------------------------------------------------
// TRIGGERS — ohne Event-Records im Index
// ---------------------------------------------------------------------------
// trig moved to kernel/trig/

// ---------------------------------------------------------------------------
// KEYBOARD — PS/2 IRQ 1, Scancode-Ringpuffer
// ---------------------------------------------------------------------------
// kbd moved to kernel/dev/

// ---------------------------------------------------------------------------
// PER-CPU
// ---------------------------------------------------------------------------
static_assert(offsetof(PerCPU, kernel_rsp) == 48, "PerCPU.kernel_rsp @48");
static_assert(offsetof(PerCPU, user_rsp)   == 56, "PerCPU.user_rsp @56");
PerCPU g_cpu;
static u64 g_boot_pml4 = 0;

// ---------------------------------------------------------------------------
// IRQ DISPATCH
// ---------------------------------------------------------------------------
volatile u64 g_tick = 0;

extern "C" u64 irq_dispatch(Regs* r){
    // Wichtig: jede IRQ entscheidet neu, ob gewechselt wird.
    // Ohne diesen Reset liest der asm-Stub nach einem Keyboard-IRQ
    // den stale-Wert vom letzten Scheduler-Wechsel und springt
    // auf einen veralteten Task-Stack.
    g_irq_next_rsp = 0;

    u32 v = (u32)r->vec;

    if (v == 33) {                       // IRQ 1: Keyboard
        kbd::irq_handler();
        pic::eoi(1);
        return (u64)r;
    }
    if (v != 32) {                        // anderer IRQ
        pic::eoi((u8)(v - 32));
        return (u64)r;
    }

    // Timer (APIC-Local-Timer, vector 32)
    apic::eoi();
    g_tick = g_tick + 1;
    this_cpu()->ticks++;

    // Alle 60 Sekunden pruefen, ob Autosync gewuenscht ist
    if ((g_tick % 6000) == 0 && persist::puts_since_sync() > 0)
        persist::request_autosync();

    // Trigger (kein Event-Record mehr!)
    if ((g_tick % 1000) == 0)
        trig::emit(1, g_tick, 0);

    task::TCB* prev = task::current;
    task::TCB* next = task::pick_next_ready();

    if (!next) return (u64)r;
    if (next == prev) { task::mark_running(prev); return (u64)r; }

    if (prev) {
        prev->rsp = (u64)r;
        {

        }
        if (prev->dead) {
            task::mark_dead(prev);
        } else {
            prev->vruntime += (20 - prev->priority) * 3;
            task::mark_ready(prev);
        }
    }
    next->vruntime += 1;
    task::mark_running(next);
    task::current = next;

    // Per-Task Kernel-Stack fuer Ring-3 -> Ring-0 (IRQ und syscall).
    // TSS.rsp0 wird von der CPU beim Wechsel aus CPL=3 nach CPL=0 geladen.
    // PerCPU.kernel_rsp wird vom syscall_entry-Stub via %gs:48 geladen.
    if (next->kstack_phys) {
        u64 ktop = (u64)mem::p2v(next->kstack_phys)
                 + next->kstack_pages * mem::pmm::PS;
        gdt::tss_set_rsp0(ktop);
        g_cpu.kernel_rsp = ktop;
    }
    // Address-Space-Switch. Kernel-Tasks (pml4_phys == 0) laufen in der
    // Boot-PML4; User-Tasks in ihrer eigenen. Immer auf den richtigen
    // Wert setzen — auch wenn wir von User- zu Kernel-Task wechseln.
    u64 want = next->pml4_phys ? next->pml4_phys : g_boot_pml4;
    if (want != hal::read_cr3()) {
        __asm__ volatile("movq %0, %%cr3" :: "r"(want) : "memory");
    }

    // Mode-Auswahl: ret-Pfad (0) fuer Tasks, die noch nie einen
    // gueltigen Kontext gespeichert haben; iretq-Pfad (1) fuer
    // bereits unterbrochene Tasks.
    //
    // Guard: Wenn prev == nullptr, kann next->rsp nicht auf einem
    // gueltigen unterbrochenen Kontext zeigen — egal was started sagt.
    // (passiert z.B. bei SMP, wenn ein zweiter Kern den ersten IRQ
    //  bekommt, bevor task::current gesetzt ist)
    u32 mode = next->started ? 1u : 0u;
    if (prev == nullptr) mode = 0;

    // Guard: pruefen, ob der iretq-Frame plausibel ist.
    // Korrekte Offsets (Regs-Layout): fr[18]=cs, fr[21]=ss (user-only).
    // Bei Korruption -> panic mit Frame-Dump (kein ret-Fallback: ret-Pfad
    // erwartet Spawn-Frame, ein echter IRQ-Frame haette 20+ Slots).
    if (mode == 1 && next->rsp) {
        u64* fr = (u64*)next->rsp;
        u64 cs = fr[18];
        if (cs != 0x1b && cs != 0x8) {
            hal::pf("[SCHED] CORRUPT cs pid=%u rsp=%p cs=%p\n",
                    (u32)next->pid, next->rsp, cs);
            hal::pf("  fr[15..19]=%p %p %p %p %p\n",
                    fr[15], fr[16], fr[17], fr[18], fr[19]);
            hal::panic("corrupt iretq frame (cs)");
        }
        if (cs == 0x1b) {
            u64 ss = fr[21];
            // 0x23 = User-Data (Frame von iretq/spawn_full)
            // 0x13 = Kernel-Data RPL3 (Frame von sysretq nach B1-Fix)
            if (ss != 0x23 && ss != 0x13) {
                hal::pf("[SCHED] CORRUPT ss pid=%u rsp=%p ss=%p\n",
                        (u32)next->pid, next->rsp, ss);
                hal::pf("  fr[15..21]=%p %p %p %p %p %p %p\n",
                        fr[15], fr[16], fr[17], fr[18], fr[19], fr[20], fr[21]);
                hal::panic("corrupt iretq frame (ss)");
            }
        }
    }

    g_irq_next_rsp  = next->rsp;
    g_irq_next_mode = mode;
    next->started   = 1;
    return (u64)r;
}

// ---------------------------------------------------------------------------
// SYSCALL — LSTAR/STAR/SFMASK + Entry-Stub (Ring 3 -> Ring 0)
// ---------------------------------------------------------------------------


// syscall_msr moved to kernel/syscall/syscall.cpp

// ============================================================================
// User-VA-Layout (A3c/A4)
// ----------------------------------------------------------------------------
// Wichtig: Diese Adressen MUESSEN im PML4-Index-Bereich [0..255] liegen
// (= User-Adressraum). PML4[256..511] ist fuer HHDM und Kernel reserviert
// und wird in alloc_pml4() mit der Boot-PML4 geteilt. User-Mappings dort
// wuerden (a) den geteilten HHDM-Bereich veraendern und (b) von
// free_user_all() nicht freigegeben, weil es nur i < 256 raeumt.
//
// Aeltere Version nutzte 0x8000000000 (= PML4[256]) — das war ein Bug,
// der pro Task 1 Page leaken liess.
// ============================================================================
static constexpr u64 USER_CODE_VA  = 0x0000000040000000ULL;   // 1 GiB, PML4[0]
static constexpr u64 USER_STACK_VA = 0x0000000050000000ULL;   // 1.25 GiB, PML4[0]
static constexpr u64 USER_ELF_BASE = 0x0000000000400000ULL;   // Linux-Default

// ============================================================================
// TEST-ONLY CODE BELOW
// ============================================================================
// Die naechsten drei emit_*-Funktionen erzeugen minimale x86_64-Programme
// als Byte-Array. Sie sind ausschliesslich fuer Kernel-interne Tests
// gedacht (ring3, a3e, r3bad). Produktiv-User-Code wird ueber ELF geladen
// (siehe elf64::load_into und spawn_elf_task).
// Wenn ein Test nicht mehr gebraucht wird: Funktion, cmd_*-Wrapper und
// Dispatch-Eintrag gemeinsam entfernen.
// ============================================================================
static void emit_user_program(u8* code, const char* msg, u32 msg_len) {
    auto w8  = [](u8* p, u8 v){ *p = v; };
    auto w32 = [](u8* p, u32 v){ p[0]=v; p[1]=v>>8; p[2]=v>>16; p[3]=v>>24; };

    u32 off = 0;
    // mov rax, 39  (sys_getpid)
    w8(code+off++, 0x48); w8(code+off++, 0xC7); w8(code+off++, 0xC0);
    w32(code+off, 39); off += 4;
    // syscall
    w8(code+off++, 0x0F); w8(code+off++, 0x05);
    // rax = pid -> in rbx speichern (callee-saved, bleibt im syscall erhalten)
    // mov rbx, rax
    w8(code+off++, 0x48); w8(code+off++, 0x89); w8(code+off++, 0xC3);

    // mov rax, 1   (sys_write)
    w8(code+off++, 0x48); w8(code+off++, 0xC7); w8(code+off++, 0xC0);
    w32(code+off, 1); off += 4;
    // mov rdi, 1   (fd=1)
    w8(code+off++, 0x48); w8(code+off++, 0xC7); w8(code+off++, 0xC7);
    w32(code+off, 1); off += 4;
    // lea rsi, [rip+disp]  -> msg @ offset 256
    w8(code+off++, 0x48); w8(code+off++, 0x8D); w8(code+off++, 0x35);
    i32 disp = (i32)256 - (i32)(off + 4);
    w32(code+off, (u32)disp); off += 4;
    // mov rdx, msg_len
    w8(code+off++, 0x48); w8(code+off++, 0xC7); w8(code+off++, 0xC2);
    w32(code+off, msg_len); off += 4;
    // syscall
    w8(code+off++, 0x0F); w8(code+off++, 0x05);

    // mov rax, 24  (sys_yield)
    w8(code+off++, 0x48); w8(code+off++, 0xC7); w8(code+off++, 0xC0);
    w32(code+off, 24); off += 4;
    // syscall
    w8(code+off++, 0x0F); w8(code+off++, 0x05);

    // mov rax, 60  (sys_exit)
    w8(code+off++, 0x48); w8(code+off++, 0xC7); w8(code+off++, 0xC0);
    w32(code+off, 60); off += 4;
    // mov rdi, rbx  (pid als exit-code)
    w8(code+off++, 0x48); w8(code+off++, 0x89); w8(code+off++, 0xDF);
    // syscall
    w8(code+off++, 0x0F); w8(code+off++, 0x05);
    // jmp $-2
    w8(code+off++, 0xEB); w8(code+off++, 0xFE);

    for (u32 i = 0; i < msg_len && (256+i) < 4096; i++)
        code[256 + i] = (u8)msg[i];
}

// A6a-Test: absichtlich ungueltiger User-Pointer (0xdeadbeef) an
// sys_write. Wenn user_range_ok korrekt arbeitet, liefert der Syscall
// E_FAULT und der User exit't mit 0xFFFFFFF2 (=-14 als u32). Ohne
// den Check wuerde der Kernel bei der Dereferenzierung crashen.
// A6a-Nachtrag: prueft, dass user_range_ok auch Kernel-Bereich
// zurueckweist (P_USER fehlt auf Zwischenebenen). Pointer 0xffff800000000000
// landet in PML4[256], das kein P_USER hat.
static void emit_kernptr_program(u8* code) {
    auto w8  = [](u8* p, u8 v){ *p = v; };
    auto w32 = [](u8* p, u32 v){ p[0]=v; p[1]=v>>8; p[2]=v>>16; p[3]=v>>24; };
    u32 off = 0;
    // mov rax, 1
    w8(code+off++, 0x48); w8(code+off++, 0xC7); w8(code+off++, 0xC0);
    w32(code+off, 1); off += 4;
    // mov rdi, 1
    w8(code+off++, 0x48); w8(code+off++, 0xC7); w8(code+off++, 0xC7);
    w32(code+off, 1); off += 4;
    // mov rsi, 0xffff800000000000 (10 48 BE imm64)
    w8(code+off++, 0x48); w8(code+off++, 0xBE);
    // little-endian imm64: 00 00 00 00 00 80 ff ff
    w8(code+off++, 0x00); w8(code+off++, 0x00); w8(code+off++, 0x00); w8(code+off++, 0x00);
    w8(code+off++, 0x00); w8(code+off++, 0x80); w8(code+off++, 0xFF); w8(code+off++, 0xFF);
    // mov rdx, 8
    w8(code+off++, 0x48); w8(code+off++, 0xC7); w8(code+off++, 0xC2);
    w32(code+off, 8); off += 4;
    // syscall
    w8(code+off++, 0x0F); w8(code+off++, 0x05);
    // cmp rax, E_FAULT (-14 als u64) -> wenn ja, exit(0xEE), sonst exit(rax)
    // mov rdi, 0xEE
    w8(code+off++, 0x48); w8(code+off++, 0xC7); w8(code+off++, 0xC7);
    w32(code+off, 0xEE); off += 4;
    // mov rax, 60
    w8(code+off++, 0x48); w8(code+off++, 0xC7); w8(code+off++, 0xC0);
    w32(code+off, 60); off += 4;
    // syscall
    w8(code+off++, 0x0F); w8(code+off++, 0x05);
    // jmp $-2
    w8(code+off++, 0xEB); w8(code+off++, 0xFE);
}

static void emit_badptr_program(u8* code) {
    auto w8  = [](u8* p, u8 v){ *p = v; };
    auto w32 = [](u8* p, u32 v){ p[0]=v; p[1]=v>>8; p[2]=v>>16; p[3]=v>>24; };
    u32 off = 0;
    // mov rax, 1
    w8(code+off++, 0x48); w8(code+off++, 0xC7); w8(code+off++, 0xC0);
    w32(code+off, 1); off += 4;
    // mov rdi, 1
    w8(code+off++, 0x48); w8(code+off++, 0xC7); w8(code+off++, 0xC7);
    w32(code+off, 1); off += 4;
    // mov esi, 0xdeadbeef  (zero-extended to rsi)
    w8(code+off++, 0xBE);
    w32(code+off, 0xDEADBEEF); off += 4;
    // mov rdx, 16
    w8(code+off++, 0x48); w8(code+off++, 0xC7); w8(code+off++, 0xC2);
    w32(code+off, 16); off += 4;
    // syscall
    w8(code+off++, 0x0F); w8(code+off++, 0x05);
    // mov rdi, rax
    w8(code+off++, 0x48); w8(code+off++, 0x89); w8(code+off++, 0xC7);
    // mov rax, 60
    w8(code+off++, 0x48); w8(code+off++, 0xC7); w8(code+off++, 0xC0);
    w32(code+off, 60); off += 4;
    // syscall
    w8(code+off++, 0x0F); w8(code+off++, 0x05);
    // jmp $-2
    w8(code+off++, 0xEB); w8(code+off++, 0xFE);
}

// A3e-Testprogramm (62 Bytes):
//   lea rdi, [rip+0x179]      ; &seed @ offset 0x180
//   movzx eax, byte [rdi]     ; eax = seed
//   mov r12, rax              ; r12 = seed (callee-saved)
//   mov [rsp-8], rax          ; write seed to user stack
//   mov ecx, 0x08000000       ; ~200ms busy loop
// spin: dec ecx; jnz spin
//   mov rax, [rsp-8]          ; read back
//   cmp r12, rax              ; ZF=1 wenn unveraendert
//   mov rdi, 0xEE             ; default: fail
//   jnz +3                    ; wenn nicht gleich, 0xEE behalten
//   mov rdi, r12              ; sonst: exit mit seed
//   mov rax, 60; syscall      ; exit
//   jmp $-2
//
// Wenn ein anderer Task den Stack ueberschrieben hat, liest dieser Task
// einen fremden Wert und exit't mit 0xEE. Wenn Isolation korrekt ist,
// exit't jeder Task mit seinem eigenen Seed.
static void emit_a3e_program(u8* code, u8 seed) {
    auto w8  = [](u8* p, u8 v){ *p = v; };
    auto w32 = [](u8* p, u32 v){ p[0]=v; p[1]=v>>8; p[2]=v>>16; p[3]=v>>24; };

    u32 off = 0;
    // lea rdi, [rip+0x179]
    w8(code+off++, 0x48); w8(code+off++, 0x8D); w8(code+off++, 0x3D);
    w32(code+off, 0x179); off += 4;
    // movzx eax, byte [rdi]
    w8(code+off++, 0x0F); w8(code+off++, 0xB6); w8(code+off++, 0x07);
    // mov r12, rax
    w8(code+off++, 0x49); w8(code+off++, 0x89); w8(code+off++, 0xC4);
    // mov [rsp-8], rax
    w8(code+off++, 0x48); w8(code+off++, 0x89); w8(code+off++, 0x44);
    w8(code+off++, 0x24); w8(code+off++, 0xF8);
    // mov ecx, 0x08000000
    w8(code+off++, 0xB9);
    w32(code+off, 0x08000000); off += 4;
    // spin: dec ecx (FF C9); jnz spin (75 FC)
    w8(code+off++, 0xFF); w8(code+off++, 0xC9);
    w8(code+off++, 0x75); w8(code+off++, 0xFC);
    // mov rax, [rsp-8]
    w8(code+off++, 0x48); w8(code+off++, 0x8B); w8(code+off++, 0x44);
    w8(code+off++, 0x24); w8(code+off++, 0xF8);
    // cmp r12, rax
    w8(code+off++, 0x49); w8(code+off++, 0x39); w8(code+off++, 0xC4);
    // mov rdi, 0xEE
    w8(code+off++, 0x48); w8(code+off++, 0xC7); w8(code+off++, 0xC7);
    w32(code+off, 0xEE); off += 4;
    // jnz +3
    w8(code+off++, 0x75); w8(code+off++, 0x03);
    // mov rdi, r12  (4C 89 E7)
    w8(code+off++, 0x4C); w8(code+off++, 0x89); w8(code+off++, 0xE7);
    // mov rax, 60
    w8(code+off++, 0x48); w8(code+off++, 0xC7); w8(code+off++, 0xC0);
    w32(code+off, 60); off += 4;
    // syscall
    w8(code+off++, 0x0F); w8(code+off++, 0x05);
    // jmp $-2
    w8(code+off++, 0xEB); w8(code+off++, 0xFE);

    // Seed byte bei Offset 0x180
    code[0x180] = seed;
}

// Ring-3-Task: baut User-Pages, springt per iretq nach CPL=3.
void user_ring3_fn(void* arg) {
    u64 mode = (u64)arg;   // 0=normal, 1=0xdeadbeef, 2=kernel-space
    const char* msg = "hello from ring 3\n";
    u32 msg_len = 18;

    u64 code_phys  = mem::pmm::alloc();
    u64 stack_phys = mem::pmm::alloc();
    u64 code_va    = USER_CODE_VA;
    u64 stack_va   = USER_STACK_VA;

    __asm__ volatile("cli");

    // Eigene PML4 bauen. Kernel-Haelfte wird geteilt, User-Haelfte
    // ist pro Task.
    u64 my_pml4 = vmm::alloc_pml4();
    if (!my_pml4) {
        hal::ps("[R3 ] alloc_pml4 fail\n");
        for(;;) __asm__ volatile("hlt");
    }

    u8* code = (u8*)mem::p2v(code_phys);
    for (int i = 0; i < 4096; i++) code[i] = 0;
    if      (mode == 1) emit_badptr_program(code);
    else if (mode == 2) emit_kernptr_program(code);
    else                emit_user_program(code, msg, msg_len);

    u8* stk = (u8*)mem::p2v(stack_phys);
    for (int i = 0; i < 4096; i++) stk[i] = 0;

    // Code und Stack in *diese* PML4 mappen, nicht in die aktuelle.
    if (!vmm::map_user_page_in(my_pml4, code_va,  code_phys,  0)) {
        hal::ps("[R3 ] map code fail\n"); for(;;) __asm__ volatile("hlt");
    }
    if (!vmm::map_user_page_in(my_pml4, stack_va, stack_phys, vmm::P_WRITE)) {
        hal::ps("[R3 ] map stack fail\n"); for(;;) __asm__ volatile("hlt");
    }

    task::TCB* self = task::current;
    if (self) self->pml4_phys = my_pml4;

    u64 user_rsp = stack_va + 4096 - 16;
    u64 user_rip = code_va;

    const char* tag = (mode == 1) ? "BADPTR"
                    : (mode == 2) ? "KERNPTR"
                    :               "normal";
    hal::pf("[R3 ] pid=%u %s code@%p stack@%p pml4=%p iretq\n",
            (u32)task::current->pid, tag, code_va, stack_va, my_pml4);

    // CR3 auf die eigene PML4 umschreiben, BEVOR wir nach Ring 3
    // springen. Der Kernel-Code und der Kernel-Stack liegen in
    // PML4[256..511], die in beiden PML4 identisch ist, also ist
    // der Switch hier sicher.
    __asm__ volatile("movq %0, %%cr3" :: "r"(my_pml4) : "memory");

    // cli; swapgs; iretq nach CPL=3.
    // RFLAGS=0x202 -> IF=1, damit Timer-IRQs auch aus Ring 3 kommen
    // (noetig fuer Praeemption und sys_exit-Aufraeumen). Beim syscall
    // loescht SFMASK IF; beim sysretq wird IF aus R11 restauriert.
    __asm__ volatile(
        "cli\n"
        "swapgs\n"
        "pushq $0x23\n"
        "pushq %[ursp]\n"
        "pushq $0x202\n"
        "pushq $0x1B\n"
        "pushq %[urip]\n"
        "iretq\n"
        :: [ursp] "r"(user_rsp), [urip] "r"(user_rip)
        : "memory"
    );
    for(;;) __asm__ volatile("hlt");
}

// Syscall-Dispatch. regs-Layout (vom Entry-Stub):
//   [0]=r9 [1]=r8 [2]=r10 [3]=rdx [4]=rsi [5]=rdi [6]=rax(nr) [7]=r11 [8]=rcx
// Linux-kompatible Errno-Werte. Rueckgabe >= 0 = Erfolg,
// Rueckgabe < 0 (als signed interpretiert) = -errno.
// errno-Werte: siehe kernel/task/fd.h


// fd-Slot allokieren. Slots 0/1/2 sind reserviert.
// Rueckgabe: fd >= 3, oder -1 wenn voll.
// fd + sys_*_kernel moved to kernel/task/fd.cpp

// syscall_dispatch moved to kernel/syscall/syscall.cpp

// syscall_entry asm moved to kernel/syscall/syscall.cpp

// ---------------------------------------------------------------------------
// SHELL — interaktive REPL auf dem Index
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// PCI — Konfigurationsraum-Enumeration
// ---------------------------------------------------------------------------
// pci moved to kernel/dev/

// ---------------------------------------------------------------------------
// VirtIO-Block (legacy, PCI)
// ---------------------------------------------------------------------------
// virtio_blk moved to kernel/dev/

// ---------------------------------------------------------------------------
// BLOCK-CACHE — LRU, 64 Slots, 32 KiB
// ---------------------------------------------------------------------------
// blockcache moved to kernel/dev/


// persist (snapshot, WAL, checkpoints) moved to kernel/persist/
// ---------------------------------------------------------------------------
// EDITOR — Kilo-Minimal, Vollbild, ANSI-Cursorposition
// ---------------------------------------------------------------------------
// editor moved to kernel/editor/

// Forward-Declarations fuer die eingebetteten Test-ELFs.
// Definition steht direkt vor kernel_main().
extern const u8 EMBEDDED_HELLO[];
extern const u64 EMBEDDED_HELLO_len;
extern const u8 EMBEDDED_FIB[];
extern const u64 EMBEDDED_FIB_len;
extern const u8 EMBEDDED_CAT[];
extern const u64 EMBEDDED_CAT_len;
extern const u8 EMBEDDED_WRITE[];
extern const u64 EMBEDDED_WRITE_len;

// shell moved to kernel/shell/

void user_a3e_fn(void*) {
    u64 code_phys  = mem::pmm::alloc();
    u64 stack_phys = mem::pmm::alloc();
    u64 code_va    = USER_CODE_VA;
    u64 stack_va   = USER_STACK_VA;

    __asm__ volatile("cli");

    u64 my_pml4 = vmm::alloc_pml4();
    if (!my_pml4) { hal::ps("[A3E] alloc_pml4 fail\n"); for(;;) __asm__ volatile("hlt"); }

    u8* code = (u8*)mem::p2v(code_phys);
    for (int i = 0; i < 4096; i++) code[i] = 0;

    task::TCB* self = task::current;
    u8 seed = (u8)('A' + (self->pid % 26));
    emit_a3e_program(code, seed);

    u8* stk = (u8*)mem::p2v(stack_phys);
    for (int i = 0; i < 4096; i++) stk[i] = 0;

    if (!vmm::map_user_page_in(my_pml4, code_va,  code_phys,  0)) {
        hal::ps("[A3E] map code fail\n"); for(;;) __asm__ volatile("hlt");
    }
    if (!vmm::map_user_page_in(my_pml4, stack_va, stack_phys, vmm::P_WRITE)) {
        hal::ps("[A3E] map stack fail\n"); for(;;) __asm__ volatile("hlt");
    }

    self->pml4_phys = my_pml4;

    u64 user_rsp = stack_va + 4096 - 16;
    u64 user_rip = code_va;

    hal::pf("[A3E] pid=%u seed='%c' code@%p stack@%p pml4=%p iretq\n",
            (u32)self->pid, seed, code_va, stack_va, my_pml4);

    __asm__ volatile("movq %0, %%cr3" :: "r"(my_pml4) : "memory");

    __asm__ volatile(
        "cli\n"
        "swapgs\n"
        "pushq $0x23\n"
        "pushq %[ursp]\n"
        "pushq $0x202\n"
        "pushq $0x1B\n"
        "pushq %[urip]\n"
        "iretq\n"
        :: [ursp] "r"(user_rsp), [urip] "r"(user_rip)
        : "memory"
    );
    for(;;) __asm__ volatile("hlt");
}

// ---------------------------------------------------------------------------
// DEMO-TASKS
// ---------------------------------------------------------------------------
// worker_a/b, sync_task_fn, init_fn, idle_fn, trig_timer, wal_apply moved to kernel/main/


extern "C" void kernel_main(){
    hal::serial_init();
    hal::ps("\n=== NEXUS v4.0 :: slab + keyboard + idle-fix ===\n\n");

    mem::hhdm = hhdm_req.response ? hhdm_req.response->offset : 0;
    if (!mm_req.response) hal::panic("no memmap");

    gdt::init();
    idt::init();
    hal::ps("[OK] gdt, idt (TSS loaded, user segs 0x1B/0x23)\n");

    // MM_USABLE-Regions aus Limine-Memmap extrahieren, an pmm geben.
    {
        static mem::Region regions[64];
        u32 nr = 0;
        auto* r = mm_req.response;
        for (u64 i = 0; i < r->count && nr < 64; i++) {
            auto* e = r->entries[i];
            if (e->type != MM_USABLE) continue;
            regions[nr].base   = e->base;
            regions[nr].length = e->length;
            nr++;
        }
        mem::pmm::init(regions, nr);
        hal::pf("[PMM] regions=%u total=%u used=%u\n",
                nr, (u32)mem::pmm::total(), (u32)mem::pmm::used_count());
    }
    hal::ps("[OK] pmm\n");

    heap::init();
    hal::ps("[OK] heap\n");

    hal::pf("[SLAB] %u Klassen bereit\n", (u32)slab::NCLASSES);

    // A3a: PML4-Clone-Mechanik verifizieren, bevor CR3-Switching drankommt.
    vmm::self_test_pml4();

    pci::init();
    virtio_blk::init();

    pic::remap();
    apic::init();
    apic::arm(100);
    hal::ps("[OK] PIC (kbd) + APIC timer 100 Hz\n");

    g_cpu.self=&g_cpu; g_cpu.cpu_id=0; g_cpu.apic_id=0;
    g_cpu.current=nullptr; g_cpu.idle=nullptr;
    g_cpu.ticks=0; g_cpu.switches=0;
    g_cpu.kernel_rsp=0; g_cpu.user_rsp=0;
    set_gs_base(&g_cpu);
    hal::wrmsr(0xC0000102, 0);          // KERNEL_GS_BASE = 0
    g_boot_pml4 = hal::read_cr3();

    // syscall MSRs setzen. Der kstack (TSS.rsp0 + g_cpu.kernel_rsp) wird
    // beim ersten Kontextwechsel aus dem TCB des aktiven Tasks geladen.
    syscall_msr::init();
    hal::pf("[OK] syscall MSRs (LSTAR=%p)\n", (u64)&syscall_entry);

    // Boot-Default-Puts: ausserhalb WAL (kein Replay von Defaults)
    persist::wal_set_recovery(true);
    idx::put(idx::NS_CONFIG | 1, idx::Type::CONFIG, 100, 0);
    idx::put(idx::NS_CONFIG | 2, idx::Type::CONFIG, 4, 0);
    persist::wal_set_recovery(false);

    fs::bitmap_load();

    // Persistenz: Snapshot laden, dann WAL replay
    {
        int n = persist::load();
        if (n > 0) hal::pf("[PERS] snapshot: %u records (seq=%u)\n",
                           (u32)n, (u32)persist::snap_seq());
        else if (n == 0) hal::ps("[PERS] no snapshot on disk\n");
        else hal::pf("[PERS] load error %d\n", n);

        if (n >= 0) {
            persist::wal_set_apply(wal_apply);
            int applied = persist::wal_replay();
            if (applied > 0)       hal::pf("[WAL ] replayed %d entries\n", applied);
            else if (applied == 0) hal::pf("[WAL ] clean (no entries)\n");
            else                   hal::pf("[WAL ] replay error %d\n", applied);
        }
    }
    // Bitmap immer aus dem Index ableiten. Die Disk-Bitmap kann nach einem
    // restore oder einem harten Kill veraltet sein.
    fs::rebuild_bitmap_from_index();
    hal::pf("[FS  ] bitmap rebuilt (%u data sectors in use)\n",
            (u32)fs::used_data_sectors());

    // A7b-3c: Migration — alte FILE/NAME ohne PATH bekommen parent_hash=hash("/")
    fs::migrate_files_v2();

    // Test-ELFs ins FS seeden (idempotent: nur wenn nicht vorhanden).
    // Damit sind alle ELF-Kommandos uniform ueber fs::cat_file erreichbar.
    {
        static char tmp[8];
        struct Seed { const char* name; const u8* data; u64 len; };
        Seed seeds[4] = {
            {"cat.elf",   EMBEDDED_CAT,   EMBEDDED_CAT_len},
            {"write.elf", EMBEDDED_WRITE, EMBEDDED_WRITE_len},
            {"hello.elf", EMBEDDED_HELLO, EMBEDDED_HELLO_len},
            {"fib.elf",   EMBEDDED_FIB,   EMBEDDED_FIB_len},
        };
        for (auto& sd : seeds) {
            if (fs::cat_file(sd.name, tmp, sizeof(tmp)) < 0) {
                int r = fs::write_file(sd.name, (const char*)sd.data, sd.len);
                hal::pf("[SEED] %s written (%d bytes)\n", sd.name, r);
            }
        }
    }

    // Idle als regulärer Task
    task::idle = task::spawn("idle", idle_fn, nullptr);
    // Idle ist der aktuelle Task im Boot-Kontext
    task::current = task::idle;
    task::mark_running(task::idle);
    // Idle = Boot-Kontext. started=1 ist korrekt, weil sein rsp
    // beim ersten IRQ in irq_dispatch auf den echten Regs-Frame
    // gesetzt wird (prev->rsp = (u64)r). Ab dann ist Idle ein
    // regulaerer unterbrochener Task, der ueber iretq fortgesetzt
    // wird. Der ret-Stack aus task::spawn() wird vor dem ersten
    // Wechsel ueberschrieben und nie benutzt.
    task::idle->started = 1;
    g_cpu.current = task::idle;
    g_cpu.idle = task::idle;

    // A5: Ab hier spawnt der Boot-Code nur noch init. init uebernimmt
    // das Starten der Dienste (shell, sync, worker-a, worker-b).
    task::spawn("init", init_fn, nullptr);

    trig::on(1, trig_timer);

    hal::pf("[IDX] %u records\n", (u32)idx::size());

    // Factory-Checkpoint einmalig anlegen
    {
        int existing = persist::checkpoint_count();
        if (existing == 0) {
            int r = persist::checkpoint_create("factory-default");
            if (r == 0) hal::ps("[CKPT] factory-default created\n");
            else        hal::pf("[CKPT] factory-default failed (%d)\n", r);
        }
    }

    llrb_test::run();
    hal::ps("--- type 'help' at the NEXUS> prompt ---\n\n");

    // IRQs freischalten
    // Timer laeuft jetzt ueber APIC, PIC-IRQ 0 bleibt maskiert.
    pic::unmask(1);                       // Keyboard

    __asm__ volatile("sti");
    hal::ps("[OK] sti, entering idle loop\n\n");

    for(;;) __asm__ volatile("pause");
}

extern "C" void __cxa_pure_virtual(){ hal::panic("pure virtual"); }
extern "C" int __cxa_atexit(void(*)(void*),void*,void*){ return 0; }
extern "C" { void* __dso_handle = nullptr; }
