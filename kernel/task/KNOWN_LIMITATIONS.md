# NEXUS Known Limitations

## Aktiv
- ~~**Syscall-Yield (nr=24) ist no-op**~~ — behoben 2025-10-22 (A8b-1).
- ~~**Syscall-Exit blockiert ~10ms**~~ — behoben 2025-10-22 (A8b-2).
- **Shell-Echo-Glitch** bei Async-Ausgabe (kosmetisch, nicht funktional).

## Behoben
- ~~**B1: `[SCHED] CORRUPT cs` bei Ring-3-Tasks**~~ — behoben 2025-10-22.
  Ursache war `sysretq` mit strukturell inkompatiblem GDT-Layout.
  Fix: `syscall_entry` nutzt jetzt `iretq` mit explizitem Frame (CS=0x1b, SS=0x23).
  Siehe `docs/state.md` und Commit `5b6141a`.

## Der Guard in `irq_dispatch`
Prüft `cs`/`ss` im iretq-Frame. Erwartet `cs ∈ {0x1b, 0x8}` und bei
`cs == 0x1b` zusätzlich `ss ∈ {0x23, 0x13}` (letzteres nach iretq-Fix
nicht mehr auftretend, aber defensiv erlaubt).
