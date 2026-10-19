# kernel/fs/ — File System (Index-Native)

**Vertrag:** Datei-Operationen über den globalen LLRB-Index (`idx/`).
Kein Verzeichnisbaum (flache Hierarchie), kein Inode-Konzept — jede
Datei ist ein `FILE`-Record mit `(size, ref)`-Paar. Extent-Chain für
Dateien > 512 B. Bitmap für Block-Allocation.

## API

### Datei-Operationen (Name-basiert)
- `write_file(name, content, len)` — erstellt/überschreibt, gibt len zurück
- `cat_file(name, out, out_max)` — liest komplette Datei in Puffer
- `ls_files()` — listet alle FILE-Records mit Name
- `rm_file(name)` — löscht FILE + NAME-Record, gibt Blöcke frei

### Block-Allocation (Bitmap)
- `bitmap_load()`, `bitmap_save()` — Dirty-Bitmap persistent
- `blk_alloc_nosave()`, `blk_alloc_run(n)`, `blk_free(sector)`
- `used_data_sectors()`, `bm_dirty_count()`, `bm_write_count()`

### Recovery
- `rebuild_bitmap_from_index()` — nach `persist::load`/`restore` gerufen
  (Disk-Bitmap kann stale sein, Index ist Source-of-Truth)

## Layout (Disk)

- **Sektor 2048–2079:** Bitmap (32 × 512 B = 16 KiB, deckt ~128k Sektoren)
- **Sektor 4096+:** Datenblöcke (Extents)
- **FILE-Record:** `key = NS_FILE | hash_name(name)`, `a = size`, `b = ref`
  - `size <= 8`: inline (Bytes in `b`)
  - `size <= 512`: `b = LBA` eines Sektors
  - `size > 512`: `b = LBA` einer Extent-Chain (FsIndex-Sektoren)
- **NAME-Record:** `key = NS_NAME | hash_name(name)`, `a/b = 16-B-Namen-Prefix`
  (Reverse-Lookup für `ls`)

## Abhängigkeiten

- `kernel/idx/` — FILE/NAME-Records, LLRB-Traversal
- `kernel/dev/virtio_blk.h` + `blockcache.h` — Sektor-I/O
- `kernel/hal/hal.h` — `hal::ps`/`pf`

## Test

- `write`/`append`/`trunc`/`writetrunc`/`cat`/`ls`/`rm` (Shell)
- `gen <name> <bytes>` — Pattern-Datei für Extent-Tests
- `wc <name>` — Größe + FNV-Hash
- `bmtest` — Bitmap-Alloc/Free-Stress
- `[FS] bitmap rebuilt` beim Boot

## Was NICHT drin ist

- Keine Verzeichnisse (flach)
- Keine Permissions (kein User/Group)
- Kein Symlink/Hardlink
- Keine Datei-Locks (fcntl)
- Kein `mmap` auf Dateien
- Kein Journaling im klassischen Sinn (WAL im `persist/`-Modul)
- Keine Defragmentierung
