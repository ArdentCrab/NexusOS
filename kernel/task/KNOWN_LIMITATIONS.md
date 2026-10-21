
## B1: err=0x28 GP-Race im iretq-Frame (nicht reproduziert)
Der Guard in irq_dispatch prueft cs/ss im iretq-Frame. Wenn cs nicht
0x1b/0x8 ist, panic statt Crash. Ursache: moeglicherweise Race zwischen
Syscall-Frame und IRQ-Frame bei hlt-Loop-Syscalls (sys_sleep, pipe_read).

**Nicht reproduziert** mit:
- bench 100 x 5 + sleep 2000 (500 Tasks, ~2.5s Scheduler)
- piper, piper-async, pipe-test
- write2 x 20 parallel

Der Race ist selten oder existiert nicht mehr. Guard bleibt als Fail-Safe.

**Falls reproduzierbar:** [SCHED] CORRUPT-Message zeigt cs/ss-Werte und
fr[15..21]. Dann ist entweder Syscall-Frame oder IRQ-Frame im next->rsp.
Fix waere: started=0 setzen in syscall_entry bei blocking syscalls.

Bei SMP wird der Race wahrscheinlicher — dann erneut testen.
