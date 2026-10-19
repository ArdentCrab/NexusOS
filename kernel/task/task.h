// kernel/task/task.h — Refactor Session 8
#ifndef KERNEL_TASK_TASK_H
#define KERNEL_TASK_TASK_H

#include "kernel/hal/hal.h"
#include "kernel/task/fd.h"

namespace task {

enum : u64 { ST_READY=1, ST_RUNNING=2, ST_DEAD=3 };

struct TCB {
    u64 pid; u64 rsp; u64 stack_phys; u64 stack_pages;
    u64 kstack_phys; u64 kstack_pages;
    u64 pml4_phys;
    u64 user_entry_va;
    u64 user_stack_top_va;
    u64 vruntime; void(*entry)(void*); void* arg;
    char name[20];
    u32 started;
    u32 dead;
    FileDesc fds[FD_MAX];

    // Scheduling
    u32 priority;    // 0..10, default 5
    u64 wake_tick;   // 0 = not sleeping (fuer S14 sleep)

    // A7b-2: current working directory
    char cwd[128];
};

extern TCB* current;
extern TCB* idle;

TCB* spawn(const char* name, void(*fn)(void*), void* arg);
TCB* spawn_full(const char* name, void(*fn)(void*), void* arg,
                u64 pml4_phys, u64 entry_va, u64 stack_top);
TCB* pick_next_ready();
void mark_running(TCB* t);
void mark_ready(TCB* t);
void mark_dead(TCB* t);
u64  reap(TCB* skip);
void dump_current();

// Syscall-API (syscall.cpp ruft diese)
i64 sys_setpriority(u64 pid, u64 prio);  // 0=OK, <0=-errno
i64 sys_getpriority(u64 pid);

i64 sys_sleep(u64 ms);            // 0..10, <0=-errno


} // namespace task

extern "C" void task_entry_stub();
extern "C" void task_entry_inner();

#endif
