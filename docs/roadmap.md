# NEXUS Roadmap

## Phase A — Fundament fertigstellen

### A10c — IO-APIC (nächster Schritt, 1–2 Sessions)
**Ziel:** Alle IRQs über IO-APIC statt PIC.

- [ ] `kernel/hal/ioapic.cpp` — MMIO @ 0xFEC00000 (QEMU q35 Standard)
- [ ] `ioapic::init(base)`, `ioapic::route(gsi, vector)`, `ioapic::eoi()`
- [ ] Keyboard: PIC IRQ1 → IO-APIC GSI 1 → Vector 33
- [ ] virtio-blk: PIC IRQ11 → IO-APIC GSI 11 → Vector 43
- [ ] `irq_dispatch`: `pic::eoi(v-32)` → `ioapic::eoi()` für GSI-Range
- [ ] PIC komplett maskieren (Ports 0x21, 0xA1 = 0xFF)
- [ ] Test: `bench 10`, `sleep`, `piper`, `sub` — alles unter IO-APIC
- [ ] Tag `v1.0-a10`

**Warum vor A8b:** IO-APIC ist Voraussetzung für SMP (Multi-Core).
A8b (echtes Syscall-Suspend) ist unabhängig und kann später.

### A8b — Syscall-Suspend (2–3 Sessions, eigene Familie)
**Ziel:** `sys_yield` und `sys_exit` blockieren nicht mehr 10 ms.

- [ ] `syscall_entry` baut SUSPEND-Frame statt direkt zurückzukehren
- [ ] `pick_next_ready` filtert SLEEPING
- [ ] `irq_dispatch` weckt bei `wake_tick`
- [ ] `sys_yield`: Scheduler-Sprung direkt aus Syscall
- [ ] `sys_exit`: Reap sofort, nicht nach Timer

### A11–A13 — Netzwerk, IPC-Verfeinerung, fd-Schichten
- [ ] Netzwerk-Stack-Vorbereitung (virtio-net, IP/UDP minimal)
- [ ] IPC-Erweiterung (Event-Prioritäten, Cancellation)
- [ ] fd-Schichten (dup, dup2, O_NONBLOCK)

## Phase B — Database-Native Unikernel (langfristig)

### B1 — Index-Based IPC
Pipes/Streams durch LLRB-Event/Trigger ersetzen.
Prozesse kommunizieren über reaktive Daten-Mutationen.

### B2 — Git-Native OS Branching
`branch` / `checkout` auf Systemzustand (Tasks, Configs, Files).
WAL als Branch-Basis, Checkpoints als Commit-Punkte.

### B3 — Time-Travel & Timeline-Forking
WAL live anhalten, zurückspulen, parallele Timelines öffnen.

### B4 — Biologische Apoptose
OOM-Killer ersetzen durch sauberes Selbstrecycling fehlerhafter Tasks
inkl. WAL-Sicherung.
