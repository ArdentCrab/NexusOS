# kernel/fs/ — Known Limitations

## F1: Flache Hierarchie
Alle Dateien im Root. Kein `/home`, kein `/etc`, keine Verzeichnisse.
Für A7 (FS-Hierarchie) muss `NS_NAME` erweitert werden (Pfad-Komponenten),
oder ein separater DIR-Record-Typ eingeführt werden. Aktuell reicht's für
Test-ELFs + User-Files (max ~1000 Dateien im 64-MiB-Image).

## F2: Name-Länge auf 16 B begrenzt
`hash_name` läuft über die ganze Name, aber `NAME`-Record speichert nur
16 B (in `a`+`b`). Bei längeren Namen sind zwei Files mit gleichem
16-B-Prefix nicht unterscheidbar in `ls`. Fix: separater Name-Block
oder NAME-Record erweitern.

## F3: Kein File-Locking
Zwei Tasks können gleichzeitig dieselbe Datei schreiben — `fd`-Semantik
ist per-Task, kein Cross-Task-Lock. `sys_open` mit `O_TRUNC` löscht den
Inhalt sofort (via `writetrunc`), während ein anderer Task noch liest.
Für Single-User-System OK, für Multi-User (A7+) muss `sys_flock` oder
`O_EXCL` kommen.

## F4: Extent-Chain max 256 Sektoren (128 KiB)
`INDEX_CHAIN_MAX = 256` in `fs::write_file`. Jeder FsIndex-Sektor hält
60 Extents à 2 MiB (theoretisch) — aber die Chain-Länge limitiert auf
256 × 2 MiB = 512 MiB. Für 64-MiB-Image irrelevant. Bei größeren Disks
muss `INDEX_CHAIN_MAX` hoch oder die Extent-Struktur geändert werden.

## F5: `blk_alloc_run` ist O(capacity)
Sucht `n` aufeinanderfolgende freie Sektoren durch sequenzielles Scannen
von `DATA_START`. Bei 64 MiB Disk (131072 Sektoren) ~1 ms — bei 1 TiB
~15 s. Fix: Free-List statt Bitmap-Scan, oder B-Baum für Runs.

## F6: Kein TRIM/Discard
Gelöschte Blöcke werden in der Bitmap freigegeben, aber nicht an das
Backing-Device (virtio-blk) gemeldet. Bei QEMU irrelevant, bei echter
SSD verschwendet es Platz. Fix: `discard`-Feature + `blk_free` erweitert.

## F7: `bitmap_save` schreibt immer alle 32 Sektoren
`g_bitmap_dirty[32]` markiert pro Sektor, aber `bitmap_save` iteriert
alle 32 und schreibt die, die dirty sind. Bei intensiver Nutzung
(alle 32 dirty) = 16 KiB Write pro FS-Operation. Optimierung: Batch
über mehrere Operationen — aber `dirty`-Flag ist schon per-Sektor,
also nur die tatsächlich geänderten werden geschrieben. Aktuell
nicht messbar (< 1 ms).

## F8: Kein Datei-Attribut (mtime/atime/ctime)
FILE-Record speichert nur `(size, ref)`. Keine Zeitstempel, keine
Permissions, keine User-ID. Für `ls -l` müssten mtime + mode in den
FILE-Record (Platz in `a`/`b` ist voll). Fix: separater ATTR-Record
oder größerer FILE-Record (>32 B → neuer Record-Typ).
