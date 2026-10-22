# NEXUS State

**Version:** v0.9-b1-final (B1 behoben)
**Datum:** 2025-10-22

## Verifizierter Funktionsumfang

### Boot & Architektur
- Limine-Boot, QEMU q35, 512 MB RAM
- GDT/IDT, TSS mit rsp0, User-Segmente 0x1B/0x23
- PMM (130086 Pages), Heap (8 MiB), Slab (8 Klassen)
- VMM mit eigenem PML4 pro Ring-3-Task

### Hardware
- virtio-blk legacy (PCI, 64 MiB Disk)
- APIC-Timer 100 Hz (Vector 32)
- PIC für Keyboard (IRQ1 → Vector 33)
- Syscall via MSR LSTAR

### Syscall-Pfad (B1-FIX)
- **Eintritt:** `SYSCALL` — CS = STAR[47:32] = 0x08 (Kernel-Code)
- **Rückkehr:** `IRETQ` mit explizitem Frame (CS=0x1b, SS=0x23)
- **Nicht mehr benutzt:** `SYSRETQ` (strukturell unvereinbar mit NEXUS-GDT)

### Blockierende Syscalls (alle funktional)
- `sys_sleep(nr=35)`: hlt-Loop, weckt bei g_tick ≥ target
- `pipe_read` / `pipe_write`: blockierend, EOF / EPIPE
- `ipc::wait_event`: blockierend mit Timeout
- `cmd_piper_async`: funktioniert

### Getestet
- `bench 10` × 3 hintereinander: 0 Panic, 0 Leak, used=2147 stabil
- `sleep 1000` → echte 1000 ms
- `piper` → pipe-roundtrip synchron
- `pipe-test` → Ring-3-Pipe über Syscall, EXIT code=104
- `sub 0 100` → blockiert echte 100 ms, dann timeout

### Persistenz
- WAL (direkt auf Disk, crash-konsistent)
- Checkpoints (factory-default), restore
- FS-Bitmap-Rebuild
- LLRB-Index, Auto-Reaping via Sync-Task

## Bekannte Einschränkungen

1. **Syscall-Yield ist no-op** — Scheduler wechselt erst beim nächsten Timer-Tick
2. **Syscall-Exit blockiert ~10 ms** — hlt-Loop bis Timer-IRQ
3. **Shell-Echo-Glitch** — bei Async-Ausgabe kollidiert User-Input mit [TASK]-Logs (kosmetisch)

## B1 — Post-Mortem

**Symptom:** `[SCHED] CORRUPT cs pid=N cs=0x2b` bei Ring-3-Tasks unter Last.

**Ursache:** `syscall_entry` benutzte `sysretq`. Das berechnet CS/SS aus
`STAR[63:48]` (CS=+16|3, SS=+8|3). NEXUS-GDT: User-CS bei 3, User-DS bei 4.
Kein STAR-Wert macht beides richtig:
- STAR=0x18 → CS=0x2b (TSS!), SS=0x23 (korrekt) → Guard-Panic
- STAR=0x08 → CS=0x1b (korrekt), SS=0x13 (Kernel-Data!) → #GP err=0x10

**Fix:** `syscall_entry` baut expliziten iretq-Frame (CS=0x1b, SS=0x23).
`SYSRETQ` entfernt. `SYSCALL` (Eintritt) unverändert — STAR[47:32]=0x08 bleibt.

**Lesson:** Diagnose-Logging am Frame-Save (`[SAVE-BAD]`) war in 5 Minuten
ergiebiger als 5 Stunden Code-Spekulation. Bei Ring-3-Crashes: erst Frame
dumpen, dann theoretisieren.

## Nächster Meilenstein
A10c — IO-APIC (siehe roadmap.md)

## A10c-2 (Phase 2, versucht und reverted, 2025-10-22)
**Versuch:** `ioapic::route(1, 33, 0)` + `pic::unmask(1)` entfernen +
`pic::eoi(1)` → `apic::eoi()` bei v==33.

**Ergebnis:** Keyboard komplett stumm. Alle Tasten ignoriert, nur Enter
kommt als leerer Prompt durch. Reproduzierbar über 2 Boots.

**Revert:** `git checkout -- nexus.cpp` (Phase 2 war nicht committet).
Build neu, Keyboard wieder da, alle Tests grün. Siehe Tag `v0.9-a10c-1-verified`.

**Nächster Ansatz für Phase 2:**
1. Debug-Print `[IRQ33] hit` in den kbd-Handler → kommt der IRQ überhaupt?
2. `[IOAPIC-REDTBL]`-Dump nach `route()` → wie sieht der Redirection-Entry aus?
3. Wenn IRQ ankommt aber Handler falsch reagiert: `ioapic::eoi()` zusätzlich
   zu `apic::eoi()` schreiben (QEMU-q35-ISA-IRQs können level-triggered sein).
4. Wenn IRQ gar nicht ankommt: prüfen ob PIC noch durchlässt, oder ob
   IO-APIC-Pin-Maskierung (Bit 16 im Redirection-Lo) korrekt ist.

## A10c-2 Diagnose (2025-10-22) — Keyboard-IRQ irrelevant
**Befund:** Vector 33 feuert nie, `[IRQ33]`-Counter bleibt 0. Trotzdem
funktioniert Keyboard (Shell-Input). Grund:

QEMU-Aufruf ist `-serial stdio` → User-Input landet auf COM1 (0x3F8),
nicht auf emulierter PS/2-Tastatur. Shell pollt COM1 in
`kbd::getchar_nb()` (shell.cpp:1128, 1561). PS/2-IRQ-Pfad
(`kbd::irq_handler` via Vector 33) ist **toter Code** im aktuellen Setup.

**Konsequenz:** IO-APIC-Routing für Keyboard ist nicht nötig.
`ioapic::route(1, 33, 0)` wird nicht gebaut — IO-APIC bleibt bereit für
echte IRQ-Geräte (virtio-blk-IRQ, Netzwerk, später SMP).

**Redirection-Table nach Reset:** alle Pins maskiert, vec=0, dest=0.
Das ist der saubere Default — IO-APIC-Routing kann später zustandsfrei
aufgesetzt werden.

**Wenn PS/2-IRQ doch mal gebraucht wird:** QEMU im GTK-Fenster nutzen
(statt `-serial stdio`), dann kommen echte PS/2-IRQs über Vector 33.
Für den aktuellen Terminal-Workflow irrelevant.

## A8b-2 (2025-10-22) — sys_exit sofort reapen
**Fix:** `sys_exit` setzt `t->dead = 1` und triggert `g_syscall_suspend`.
`task_syscall_suspend` erkennt `prev->dead`, markiert dead und wählt next.
Fallback auf `task::idle`, falls kein anderer Task ready ist.

**Verifiziert:** `bench 100` + `bench 10` ohne Crash, alle EXIT/REAP.
`sys_yield` und `sys_exit` wechseln sofort, kein hlt-Wait mehr.

**Nebenbefund:** ~100 Pages einmaliger Leak bei `bench 100` (Baseline 2132 → 2232).
Nicht kumulativ (zweiter bench-Aufruf ändert nicht weiter). Verdacht: bench-Setup
selbst. Wird in eigener Session untersucht (A8b-4+).

**Offen (Session 3):** `started=0`-Tasks im Suspend-Pfad. Wenn ein gerade
gespawnter Task (Ret-Path) sofort `sys_yield`/`sys_exit` macht, bevor er
einmal gelaufen ist, könnte der Ret-Stack als iretq-Frame missinterpretiert
werden. In der Praxis selten (erster Syscall ist meistens sys_write), aber
sollte sauber abgedeckt werden.
