# kernel/mm/ — Known Limitations

## M1: Heap wächst, schrumpft nicht
`heap::free` gibt Speicher an die Free-List zurück, aber Pages werden nie
an `pmm` zurückgegeben. `bench 20` hinterlässt ~82 KiB im Heap (20 TCBs +
FileDesc-Arrays) — `pmm used=2122` ist dabei sauber. Fix: Heap-Compaction
oder explizite `heap_shrink()`, wenn Free-List einen Schwellwert übersteigt.
Nicht akut (8 MiB Heap reicht für ~1000 Tasks), aber ein Langzeit-Leak
bei Dauerbetrieb mit vielen kurzlebigen Tasks.

## M2: Heap-Alignment ist nominal
`heap::alloc(n, al)` richtet `n` auf `al` aus, aber der zurückgegebene
Pointer ist nur durch `sizeof(Block)` (= 64 B) alignment-garantiert.
Für `al <= 8` OK, für `al = 16` (Default) auch OK (64 ist 16-aligned).
Für `al = 32` oder `al = 64` — nicht garantiert. Bei SSE/AVX aus User-Code
könnte das crashen. Aktuell kein Problem (Kernel nutzt `-mno-sse`), aber
für S10 (`test_helpers/`) relevant, wenn AVX2-Tests kommen.

## M3: pmm-Regions-Anzahl schwankt
QEMU/Limine liefert pro Boot 3 MM_USABLE-Regions, aber `pmm.total` schwankt
um ±20 Pages (130131–130150 über Sessions beobachtet). Das ist ACPI-Table-
Placement, nicht unser Code. `used` ist stabil (2122 Baseline). Dokumentiert,
nicht gefixt — wer `used` für Leak-Detection nutzt, ist auf der sicheren
Seite.

## M4: MAX_PAGES = 2^20 (4 GiB)
`pmm::MAX = 1ULL << 20`. Bei >4 GiB physischem RAM werden Pages oberhalb
ignoriert. Bei 512 MiB (QEMU-Default) unkritisch. Fix: `MAX_PAGES` an
`regions[i].base + regions[i].length` anpassen oder dynamisch allokieren.

## M5: Kein Demand-Paging
`elf64::load_into` allokiert Pages eager für jedes PT_LOAD-Segment. Bei
großen ELFs (z.B. statisch gelinkte Rust-Binaries mit 10+ MiB) verbraucht
das sofort `pmm`-Pages, auch wenn die Pages nie berührt werden. Fix:
Page-Fault-Handler + Lazy-Allocation — gehört zu A8 (Memory-Management).

## M6: Kein COW (Copy-on-Write)
`sys_open` lädt Datei-Inhalt eager in `ram_buf`. Bei großen Dateien
>1 MiB (KNOWN LIMITATION P2 in `persist/`) wächst der Heap. Für
File-Sharing (mehrere Tasks öffnen dieselbe Datei) fehlt COW — jeder
Task bekommt eine eigene Kopie. Fix: Shared Pages + Reference-Counting.

## M7: `free_user_all` räumt nur PML4[0..255]
`free_user_all(pml4_phys)` iteriert nur die User-Half (i < 256). Die
Kernel-Half (PML4[256..511]) ist per Design in allen PML4s identisch
(geteilt mit Boot-PML4) und wird nicht freigegeben. Korrekt — aber
wenn jemand versehentlich User-Mappings in PML4[256+] macht, werden
sie nicht freigegeben. Der `[VMM] pml4 self-test` (user-dirty=0)
prüft das beim Boot.
