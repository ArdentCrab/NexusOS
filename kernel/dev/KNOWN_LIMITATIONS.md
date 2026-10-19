# kernel/dev/ — Known Limitations

## D1: VirtIO Legacy nur
`disable-modern=on` in QEMU. Real-Hardware ab ~2015 ist oft modern-only.
Migration auf VirtIO 1.0 (PCI-Capability, MMIO-Bars) ist ein eigenes
Projekt (~500 LOC). Für QEMU-basierte Entwicklung kein Problem, für
Bare-Metal auf echter Hardware blockierend.

## D2: Kein MSI/MSI-X
Alle IRQs laufen über klassische PIC-Leitungen. Das limitiert auf 16 IRQs
und Shared-Interrupt-Handling. Für S7+ (APIC/IO-APIC, A10) muss das
umgebaut werden.

## D3: blockcache ist kein echtes LRU
`evict()` scannt alle 64 Slots nach dem kleinsten `age`-Wert. Bei
64 Slots OK (O(64) pro Eviction), bei 1024+ spürbar. Fix: sortierte
Liste oder Clock-Algorithmus.

## D4: Kein DMA
`virtio_blk::blk_request_locked` kopiert Daten byte-weise durch
`req_data` (1 Sektor pro Request). Bei >1 Sektor pro Request fehlt
das Descriptor-Chain-Setup. Aktuell unkritisch (nur 1-Sektor-Reads),
aber `fs::write_file` bei großen Dateien macht viele Einzel-Requests.

## D5: kbd unterstützt nur Scancode Set 1
Moderne Tastaturen nutzen Set 2 oder 3. Set 1 ist der BIOS-Default
(QEMU liefert Set 1), aber echte USB-Tastaturen über PS/2-Adapter
können Set 2 liefern. Fix: Set-Detection beim Init, oder USB-HID-Stack.

## D6: `getchar_nb_wait` ist timing-abhängig
`for (int i = 0; i < 3000; i++)` mit 50 Pause-Iterationen dazwischen.
Bei QEMU mit 100 Hz PIT funktioniert das, bei schnellerer CPU könnte
die Wartezeit zu kurz sein. Fix: PIT-basiertes Timeout statt busy-loop.
