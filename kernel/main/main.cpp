// kernel/main/main.cpp — Refactor Session 11c-light
#include "kernel/main/main.h"
#include "kernel/mm/mm.h"
#include "kernel/hal/hal.h"
#include "kernel/idx/idx.h"
#include "kernel/persist/persist.h"
#include "kernel/shell/shell.h"
#include "kernel/task/task.h"

extern volatile u64 g_tick;

static void worker_a(void*){
    u64 next = 500;
    for(;;){
        if (g_tick >= next) {
            hal::klogf(hal::LOG_DEBUG, "[A] tick=%u idx=%u slab=%uK heap=%uK\n",
                    (u32)g_tick, (u32)idx::size(),
                    (u32)(slab::bytes_used()/1024),
                    (u32)(heap::used()/1024));
            next = g_tick + 500;
        }
        for(int i=0;i<3000000;i++) __asm__ volatile("" ::: "memory");
    }
}
static void worker_b(void*){
    u64 next = 750;
    for(;;){
        if (g_tick >= next) {
            hal::klogf(hal::LOG_DEBUG, "[B] tick=%u idx=%u slab=%uK heap=%uK\n",
                    (u32)g_tick, (u32)idx::size(),
                    (u32)(slab::bytes_used()/1024),
                    (u32)(heap::used()/1024));
            next = g_tick + 750;
        }
        for(int i=0;i<5000000;i++) __asm__ volatile("" ::: "memory");
    }
}
// Forward-Decls fuer init_fn: shell::task_fn, sync_task_fn, worker_a/b
// sind spaeter im File definiert.
static void sync_task_fn(void*);
static void worker_a(void*);
static void worker_b(void*);

// ============================================================================
// A5: Init-Prozess (Boot-Orchestrator)
// ============================================================================
// PID 2 (idle bleibt PID 1). Liest NS_CONFIG|1 und spawnt die
// Hintergrunddienste. Der Kernel-Boot-Code spawnt nur noch init.
//
// Config-Semantik (aktuell):
//   cfg <  50  -> nur shell + sync (minimaler Boot)
//   cfg >= 50  -> + worker-a (Debug)
//   cfg >= 100 -> + worker-b (Debug, Default)
//
// init laeuft nach dem Spawnen im hlt-Loop. Er wird nicht gereapt
// (er ist dead=0), bleibt also dauerhaft PID 2.
void init_fn(void*) {
    // Config lesen
    idx::Rec* cfg = idx::find(idx::NS_CONFIG | 1);
    u64 cfg_val = cfg ? cfg->a : 100;
    hal::pf("[INIT] pid=%u cfg=NS_CONFIG|1=%u\n",
            (u32)task::current->pid, (u32)cfg_val);

    // Kern-Dienste (immer)
    task::spawn("shell", shell::task_fn, nullptr);
    task::spawn("sync",  sync_task_fn,   nullptr);
    hal::ps("[INIT] spawned shell + sync\n");

    // Debug-Tasks abhaengig von Config
    if (cfg_val >= 50) {
        task::spawn("worker-a", worker_a, nullptr);
        hal::ps("[INIT] spawned worker-a\n");
    }
    if (cfg_val >= 100) {
        task::spawn("worker-b", worker_b, nullptr);
        hal::ps("[INIT] spawned worker-b\n");
    }

    // Idle-Loop (init lebt weiter)
    for (;;) __asm__ volatile("sti; hlt");
}

void idle_fn(void*){ for(;;) __asm__ volatile("sti; hlt"); }



void trig_timer(u64, u64 a, u64){
    hal::klogf(hal::LOG_DEBUG, "[TRG] tick %u\n",(u32)a);
}

// ---------------------------------------------------------------------------
// WAL-Apply (aus persist::wal_replay gerufen)
// ---------------------------------------------------------------------------
void wal_apply(u8 op, u8 type, u64 key, u64 a, u64 b) {
    if (type == 0 || type == 2 || type == 3) return;
    if (op == 1) {
        idx::remove(key);
    } else {
        idx::put(key, (idx::Type)type, a, b);
    }
}

// ---------------------------------------------------------------------------
// SYNC-TASK — laeuft im Hintergrund, triggert Auto-Sync
// ---------------------------------------------------------------------------
static void sync_task_fn(void*) {
    for (;;) {
        if (persist::autosync_pending()) {
            u64 puts = persist::puts_since_sync();
            hal::klogf(hal::LOG_INFO, "[SYNC] autosync after %u puts\n", (u32)puts);
            persist::autosync_now();
            hal::klogf(hal::LOG_INFO, "[SYNC] done, seq=%u\n",
                       (u32)persist::snap_seq());
        }
        task::reap(task::current);
        for (int i = 0; i < 500000; i++) __asm__ volatile("" ::: "memory");
    }
}

// ---------------------------------------------------------------------------
// cat.elf (A6b-3b): open+read+write+close aus Ring 3.
// cat.elf (A6b-3b): open+read+write+close aus Ring 3.
// [S1: moved to elf_blobs.cpp] EMBEDDED_CAT[]
// [S1: moved to elf_blobs.cpp] EMBEDDED_CAT_len

// write.elf (A6c): open+write+close aus Ring 3.
// [S1: moved to elf_blobs.cpp] EMBEDDED_WRITE[]
// [S1: moved to elf_blobs.cpp] EMBEDDED_WRITE_len

// Eingebettete Test-ELFs (generiert zur Build-Zeit aus /tmp/a4_build)
//
// KNOWN LIMITATION (T3): Jedes Test-ELF waechst das Kernel-Binary um
// seine Groesse. Bei >2 Testprogrammen sollte dieser Pfad durch
// "schreibe Test-ELFs beim Boot in fs::write_file" ersetzt werden.
// ---------------------------------------------------------------------------
extern const u8 EMBEDDED_HELLO[];
// [S1: moved to elf_blobs.cpp] EMBEDDED_HELLO[]
// [S1: moved to elf_blobs.cpp] EMBEDDED_HELLO_len
extern const u8 EMBEDDED_FIB[];
// [S1: moved to elf_blobs.cpp] EMBEDDED_FIB[]
// [S1: moved to elf_blobs.cpp] EMBEDDED_FIB_len

// ---------------------------------------------------------------------------
// KERNEL MAIN
// ---------------------------------------------------------------------------