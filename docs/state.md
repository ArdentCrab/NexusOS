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
