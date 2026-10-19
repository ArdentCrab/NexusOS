# kernel/dev/ — PCI, VirtIO-Block, Block-Cache, Keyboard

**Vertrag:** Hardware-Treiber-Schicht. Vier unabhängige Module, die
gemeinsam Block-I/O und Tastatur-Eingabe bereitstellen.

## Module

### pci — PCI-Konfigurationsraum
- `scan(visitor, ctx)` / `count()` / `init()`
- `cfg_read32/16`, `cfg_write32` — inline Port-I/O (0xCF8/0xCFC)
- `struct Device` — bus/dev/func, vendor/device, class, BARs + Größen
- Abhängigkeit: keine außer `hal.h`

### virtio_blk — VirtIO Legacy Block Device
- `init()`, `capacity()`, `is_ready()`
- `read_sector(lba, buf)`, `write_sector(lba, buf)` — 512-Byte-Sektoren
- Queue-Setup, Descriptor-Chain, Request-Buffer (hdr/data/status)
- Abhängigkeit: `pci/`, `mm/` (pmm::alloc_n + p2v)
- **Legacy-Interface** (disable-modern=on in QEMU), kein VirtIO 1.0

### blockcache — LRU Cache, 64 Slots × 512 B
- `read/write(lba, buf)` — cache-aware
- `flush()` — schreibt dirty Sektoren
- `dirty_count()` / `valid_count()` / `hits/misses/evicted`
- Abhängigkeit: `virtio_blk/`

### kbd — PS/2 Tastatur + serieller Input
- `irq_handler()` — von `irq_dispatch` bei IRQ 1 gerufen
- `getchar_nb()` — PS/2-Ringpuffer + serieller Port (QEMU -serial)
- `getchar_nb_wait()` — für ESC-Sequenzen (Pfeiltasten)
- Scancode Set 1, US-Layout, Shift/Caps
- Abhängigkeit: `hal.h`

## Test

- `pci` → 7 Funktionen (QEMU q35)
- `blk` → 131072 Sektoren (64 MiB)
- `blk read <sec>` → Hexdump eines Sektors
- `edit <file>` → interaktiver Test von `getchar_nb_wait`

## Was NICHT drin ist

- Kein VirtIO 1.0 (modern), nur Legacy-Interface
- Kein MSI/MSI-X, nur klassische IRQs
- Kein Multi-Queue (queue0 only)
- Kein DMA-Setup (nur 32-bit Legacy-Addressing, phys>>12)
- Kein USB, kein AHCI, kein NVMe
- Kein vollständiges Scancode-Set (nur Set 1, US-Layout)
