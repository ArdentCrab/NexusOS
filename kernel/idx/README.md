# kernel/idx/ — LLRB Key-Value Index

**Vertrag:** Persistenter, geordneter Key-Value-Index. Thread-safe via Spinlock.
Keys sind 64-bit mit Namespace-Prefix in den obersten 8 Bits (NS_*).

## API

- `put(key, type, a, b)` — Insert/Update. Bei existierendem Key: update a/b/type in-place.
- `find(key)` — Lookup. nullptr wenn nicht vorhanden.
- `remove(key)` — Delete mit physischer Node-Freigabe (Content-Swap-Verfahren).
- `scan(type, visitor, ctx)` / `scan_all(visitor, ctx)` — In-Order-Traversal.
- `compute_hash(mode, filter, &count)` — FNV-basierter State-Hash für Checkpoints.

## Abhängigkeiten

- `kernel/hal/hal.h` — Spinlock, `hal::pf`/`panic`
- `kernel/idx/wal_hooks.h` — die drei `nexus_wal_*`-Hooks (Implementierung: nexus.cpp, später persist/)
- `operator new`/`operator delete` — aus `kernel/mm/`

## Test

`llrb_test::run()` — 500 Inserts / 250 Deletes / Cleanup. Prüft alle LLRB-Invarianten
(Black-Height, Red-Red, Sortierung). Läuft bei jedem Boot, Laufzeit ~5ms.

## Was NICHT drin ist

- Keine Iteratoren (nur Visitor-Callbacks)
- Kein Bulk-Insert
- Kein Range-Query (nur `scan(Type)`, kein `scan(key_min, key_max)`)
