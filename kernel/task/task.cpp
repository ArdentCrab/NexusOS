// kernel/task/task.cpp — Refactor Session 8
#include "kernel/task/task.h"
#include "kernel/ipc/pipe.h"
#include "kernel/idx/idx.h"
#include "kernel/mm/mm.h"

extern volatile u64 g_tick;

namespace task {
static u64 next_pid = 1;
TCB* current = nullptr;
TCB* idle = nullptr;
extern "C" void task_entry_stub();

// spawn_full erlaubt das Setzen von pml4/entry/stack_top, BEVOR der
// Task in den Index eingetragen wird. Das verhindert ein Race: der
// Scheduler koennte einen Task picken, dessen User-Felder noch leer sind.
TCB* spawn_full(const char* name, void(*fn)(void*), void* arg,
                       u64 pml4_phys, u64 entry_va, u64 stack_top) {
    TCB* t = new TCB{};
    t->pid = next_pid++;
    t->entry = fn; t->arg = arg;
    int i=0; for(;name[i]&&i<19;i++) t->name[i]=name[i];
    u64 pages = 4;
    t->stack_pages = pages;
    t->stack_phys = mem::pmm::alloc_n(pages);
    t->kstack_pages = 4;
    t->kstack_phys  = mem::pmm::alloc_n(t->kstack_pages);
    u64 top = (u64)mem::p2v(t->stack_phys) + pages*mem::pmm::PS;
    u64* sp = (u64*)top;
    *--sp = (u64)&task_entry_stub;
    *--sp = 0; *--sp = 0; *--sp = 0;
    *--sp = 0; *--sp = 0; *--sp = 0;
    t->rsp = (u64)sp;
    t->started = 0;
    t->priority = 5;   // default
    t->wake_tick = 0;
    t->cwd[0] = '/'; t->cwd[1] = 0;
    for (u32 i = 0; i < ipc::SUB_MAX; i++) t->subs[i].used = false;
    t->pml4_phys         = pml4_phys;
    t->user_entry_va     = entry_va;
    t->user_stack_top_va = stack_top;
    idx::put(idx::NS_TASK | t->pid, idx::Type::TASK, (u64)t, ST_READY);
    return t;
}
TCB* spawn(const char* name, void(*fn)(void*), void* arg){
    return spawn_full(name, fn, arg, 0, 0, 0);
}
struct PickCtx { TCB* best; };
static void pick_visit(idx::Rec* r, void* v){
    PickCtx* c = (PickCtx*)v;
    if (r->b != ST_READY) return;
    TCB* t = (TCB*)r->a;
    if (!c->best) { c->best = t; return; }
    // vruntime asc (primaer), priority desc (tiebreaker)
    if (t->vruntime < c->best->vruntime) { c->best = t; return; }
    if (t->vruntime > c->best->vruntime) return;
    if (t->priority > c->best->priority) c->best = t;
}
TCB* pick_next_ready(){
    PickCtx c{nullptr};
    idx::scan(idx::Type::TASK, pick_visit, &c);
    return c.best;
}
__asm__(
    ".global task_entry_stub\n"
    "task_entry_stub:\n"
    "  sti\n"
    "  call task_entry_inner\n"
    "1: hlt\n  jmp 1b\n");
extern "C" void task_entry_inner();
void dump_current() {
    TCB* t = current;
    if (!t) { hal::ps("  task: <null>\n"); return; }
    hal::pf("  task: pid=%u name=%s started=%u dead=%u kstack=%p\n",
            (u32)t->pid, t->name, t->started, t->dead, t->kstack_phys);
}
void mark_running(TCB* t){ idx::put(idx::NS_TASK | t->pid, idx::Type::TASK, (u64)t, ST_RUNNING); }
void mark_ready  (TCB* t){ idx::put(idx::NS_TASK | t->pid, idx::Type::TASK, (u64)t, ST_READY); }
void mark_dead   (TCB* t){ idx::put(idx::NS_TASK | t->pid, idx::Type::TASK, (u64)t, ST_DEAD); }

struct ReapBatch {
    u64 keys[32]; u64 stacks[32]; u64 pages[32]; TCB* tlbs[32];
    u32 count; TCB* skip;
};
static void reap_collect(idx::Rec* r, void* v){
    ReapBatch* c = (ReapBatch*)v;
    if (r->b != ST_DEAD) return;
    TCB* t = (TCB*)r->a;
    if (t == c->skip) return;
    if (c->count >= 32) return;
    u32 i = c->count++;
    c->keys[i]   = r->key;
    c->stacks[i] = t->stack_phys;
    c->pages[i]  = t->stack_pages;
    c->tlbs[i]   = t;
}
u64 reap(TCB* skip){
    ReapBatch b{};
    b.skip = skip;
    u64 rflags;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(rflags) : : "memory");
    idx::scan(idx::Type::TASK, reap_collect, &b);
    for (u32 i = 0; i < b.count; i++) {
        if (b.stacks[i] && b.pages[i])
            mem::pmm::free_n(b.stacks[i], b.pages[i]);
        TCB* t = b.tlbs[i];
        u64 used0 = mem::pmm::used_count();
        u64 pml4_freed = 0, kstack_freed = 0;
        // fd-Table raeumen: erst flushen (dirty -> Disk), dann Puffer frei.
        // Ohne Flush wuerden User-Schreibvorgaenge beim Task-Tod verloren
        // gehen, bevor sys_close gerufen wurde.
        if (t) {
            for (int fd = 3; fd < FD_MAX; fd++) {
                if (t->fds[fd].in_use) {
                    if (t->fds[fd].kind != 0) {
                        ipc::pipe_close((int)t->fds[fd].pipe_idx,
                                        t->fds[fd].kind == 1);
                    } else if (t->fds[fd].dirty) {
                        fd_flush(fd);
                    }
                    if (t->fds[fd].ram_buf) {
                        heap::free((void*)t->fds[fd].ram_buf);
                        t->fds[fd].ram_buf = 0;
                    }
                    t->fds[fd].in_use = 0;
                    t->fds[fd].dirty = 0;
                    t->fds[fd].kind = 0;
                }
            }
        }
        if (t && t->kstack_phys && t->kstack_pages) {
            mem::pmm::free_n(t->kstack_phys, t->kstack_pages);
            kstack_freed = t->kstack_pages;
        }
        if (t && t->pml4_phys) {
            u64 before = mem::pmm::used_count();
            vmm::free_user_all(t->pml4_phys);
            pml4_freed = before - mem::pmm::used_count();
        }
        u64 used1 = mem::pmm::used_count();
        // kstack_pages + 4 stack_pages (kernel task stack) sollten auf
        // jeden Fall freigegeben werden.
        if (t) {
            hal::pf("[REAP] pid=%u stack=%u kstack=%u pml4tree=%u total=%u "
                    "used %u -> %u\n",
                    (u32)t->pid,
                    (u32)t->stack_pages, (u32)kstack_freed,
                    (u32)pml4_freed, (u32)(kstack_freed + t->stack_pages + pml4_freed),
                    (u32)used0, (u32)used1);
        }
        idx::remove(b.keys[i]);
        delete t;
    }
    if (rflags & (1ULL<<9)) __asm__ volatile("sti" ::: "memory");
    return b.count;
}
} // namespace task

extern "C" void task_entry_inner(){
    task::TCB* t = task::current;
    if (!t) { hal::ps("[TASK] current NULL\n"); for(;;) __asm__ volatile("hlt"); }
    hal::pf("[TASK] start pid=%u name=%s\n", (u32)t->pid, t->name);
    t->entry(t->arg);
    // dead=1 signalisiert dem Scheduler, nicht zu requeuen.
    // Der Scheduler setzt ST_DEAD im Index (mark_dead), damit der
    // Race zwischen dead=1 und Index-Update geschlossen ist.
    t->dead = 1;
    for(;;) __asm__ volatile("sti; hlt");
}


// Syscall-API: Prioritaet setzen/lesen
namespace task {

i64 sys_setpriority(u64 pid, u64 prio) {
    if (prio > 10) return (i64)-22;  // EINVAL
    idx::Rec* r = idx::find(idx::NS_TASK | pid);
    if (!r || r->type != (u32)idx::Type::TASK) return (i64)-3;  // ESRCH
    TCB* t = (TCB*)r->a;
    if (!t) return (i64)-3;
    t->priority = (u32)prio;
    return 0;
}

i64 sys_getpriority(u64 pid) {
    idx::Rec* r = idx::find(idx::NS_TASK | pid);
    if (!r || r->type != (u32)idx::Type::TASK) return (i64)-3;
    TCB* t = (TCB*)r->a;
    if (!t) return (i64)-3;
    return (i64)t->priority;
}

// sys_sleep(ms) — kooperatives Sleep via hlt-Loop.
// - Waehrend hlt IRQs enabled: Scheduler laeuft, andere Tasks progress.
// - Unser Task wird von IRQ preempted und am hlt-Punkt resumed.
// - Stack waechst nicht (IRQ balanced push/pop).
// - Cap 30000 ms um Endlos-Hang zu verhindern.
i64 sys_sleep(u64 ms) {
    if (!current) return (i64)-22;   // EINVAL
    if (ms < 10) return 0;           // zu kurz, nichts zu tun
    if (ms > 30000) ms = 30000;      // 30s Cap
    u64 target = g_tick + (ms / 10);
    while (g_tick < target) {
        __asm__ volatile("sti; hlt");
    }
    return 0;
}

} // namespace task (reopened)
