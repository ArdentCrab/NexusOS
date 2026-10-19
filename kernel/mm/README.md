# kernel/mm/ — Memory Management

**Vertrag:** Physische + virtuelle Speicherverwaltung. Vier Module, die
gemeinsam Page-Allocation und Adressraum-Setup bereitstellen.

## Module

### pmm — Physical Memory Manager
- `init(regions, n)` — Bitmap-Allocator, 1 Page = 4 KiB, MAX = 2^20 Pages
- `alloc_n(n)`, `alloc()`, `free_n(base, n)`
- `total()`, `used_count()`, `free_count()`
- Abhängigkeit: `hal::panic` (kein p2v — arbeitet nur mit Phys-Adressen)
- **Regions-Anzahl schwankt** (QEMU/Limine ACPI-Placement): 3 Regions, total ±20 Pages pro Boot

### slab — Small-Object Allocator
- 6 Klassen: 32, 64, 128, 256, 512, 1024 B
- `alloc(usize)`, `free(ptr)` — wählt passende Klasse, sonst Heap-Fallback
- `total()`, `used()`, `bytes_used()`, `dump()`
- Abhängigkeit: `pmm` (grow via `heap::alloc`), `heap`
- **`NCLASSES = 6`** via `static_assert` an `classes[]` gekoppelt

### heap — Large-Object Allocator
- First-Fit mit Coalesce, 8 MiB initial (2048 Pages)
- `init()`, `alloc(n, al=16)`, `free(ptr)`
- `used()`, `total()`
- Abhängigkeit: `pmm` (init + grow), `hal::pf`
- **Wächst, schrumpft nicht** — `bench 20` hinterlässt ~82 KiB im Heap
  (20 TCBs à 2 KiB), pmm-seitig sauber

### vmm — Virtual Memory Manager
- `p2v(phys)` inline über `hhdm` (Higher-Half Direct Map)
- `get_phys(va)`, `map_user_page(va, phys, flags)`, `map_user_page_in(pml4, ...)`
- `alloc_pml4()`, `free_pml4()`, `pte_phys_in()`, `user_range_ok()`, `free_user_all()`
- `self_test_pml4()` — Boot-Selbsttest (kernel-shared, diff, user-dirty)
- Abhängigkeit: `pmm` (Page-Allocs für PTs), `hal::read_cr3`

## Test

- `[VMM] pml4 self-test: kernel-shared=2 diff=0 user-dirty=0` (Boot)
- `[PMM] regions=3 total=... used=...` (Boot)
- `[HEAP] 8192 KiB` (Boot)
- `[SLAB] 6 Klassen bereit` (Boot)
- `mem` in Shell

## Was NICHT drin ist

- Kein Demand-Paging (nur Pre-Fault)
- Kein Swap
- Kein NUMA
- Kein Memory-Mapping (mmap/munmap)
- Kein COW (Copy-on-Write)
- Kein hugepage-Support
