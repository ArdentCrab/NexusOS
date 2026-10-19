# kernel/idx/ — Known Limitations

## L1: Kein Range-Query
`scan(Type)` filtert nur nach Record-Typ, nicht nach Key-Range. Für
`fs::ls` reicht das, für zukünftige FS-Hierarchie (A7) wird
`scan_range(key_min, key_max, visitor)` gebraucht.

## L2: Kein Bulk-Insert
Jeder `put` nimmt den Spinlock einzeln. `persist::load` mit 500 Records
macht 500 Lock/Unlock-Zyklen. Bei größeren Snapshots spürbar (aktuell
nicht messbar, < 1ms).

## L3: `HASH_TAG` ist statisch
`compute_hash` mischt `HASH_TAG = 0x4E4558555332` ein. Bei Format-Änderung
des Index (neue Record-Typen, andere Feld-Semantik) muss `HASH_TAG` inkrementiert
werden — sonst passen alte Checkpoints stumm weiter.

## L4: Rekursionstiefe
`ins`, `del`, `check_black` sind rekursiv. Bei ~2^32 Records könnte der
Kernel-Stack (16 KiB) überlaufen. Praktisch irrelevant, weil
`pmm.total ~ 130k` Pages ≈ 512 MiB physisch, also max ~1M Records.
