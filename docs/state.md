# NEXUS State

**Version:** v0.9-a11-4b
**Datum:** 2025-10-23

## Verifizierter Funktionsumfang

### Boot & Architektur
- Limine-Boot, QEMU q35, 512 MB RAM
- GDT/IDT, TSS mit rsp0, User-Segmente 0x1B/0x23
- PMM (~130k Pages), Heap (8 MiB), Slab (8 Klassen)
- VMM mit eigenem PML4 pro Ring-3-Task
- LLRB-Index als globaler Zustandsbaum, WAL + Checkpoints

### Hardware
- virtio-blk legacy (PCI, 64 MiB Disk)
- virtio-net legacy (PCI, MAC 52:54:00:12:34:56, RX/TX-Queues)
- APIC-Timer 100 Hz (Vector 32)
- IO-APIC erkannt (v0.9-a10c-1), aber nicht aktiv genutzt
- Keyboard via COM1-Polling (`-serial stdio`), PS/2-IRQ-Pfad ungenutzt
- Syscall via MSR LSTAR

### Syscall-Pfad (B1-FIX, seit v0.9-b1-final)
- Eintritt: `SYSCALL` — CS = STAR[47:32] = 0x08 (Kernel-Code)
- Rückkehr: `IRETQ` mit explizitem Frame (CS=0x1b, SS=0x23)
- Suspend-Pfad (A8b-3): bei `sys_yield`/`sys_exit` baut `syscall_entry`
  einen IRQ-kompatiblen Frame, ruft `task_syscall_suspend`, springt in
  Scheduler. Mode-Check (ret-path vs iretq) für spawn-frame Tasks.
- **Nicht mehr benutzt:** `SYSRETQ` (strukturell unvereinbar mit NEXUS-GDT)

### Blockierende Syscalls (alle funktional)
- `sys_sleep(nr=35)`: hlt-Loop, weckt bei g_tick ≥ target
- `pipe_read` / `pipe_write`: blockierend, EOF / EPIPE
- `ipc::wait_event`: blockierend mit Timeout
- `sys_yield(nr=24)`: suspendiert sofort (kein hlt-Wait)
- `sys_exit(nr=60)`: suspendiert sofort (kein hlt-Wait)

### Netzwerk-Stack (A11.1-4)
- **L2**: virtio-net legacy, RX pre-filled (8 × 2048 B), TX mit virtio-net-Header
- **ARP**: request + cache (16 Slots), `arp <ip>` Shell-Befehl
- **IPv4**: Header build/parse, Checksum, `ip_send` mit ARP-Resolution
- **ICMP**: `ping <ip>` Shell, `sys_net_ping(nr=200)` für Ring-3
- **UDP**: `net::udp_open/send/recv/close/drain`, Syscalls 202-205
- **Shell**: `netinfo`, `arp`, `ping`, `r3ping`, `udpsend`, `udprecv`

### Persistenz
- WAL (direkt auf Disk, crash-konsistent)
- Checkpoints (factory-default), restore
- FS-Bitmap-Rebuild
- LLRB-Index, Auto-Reaping via Sync-Task

### Getestet (2025-10-22/23)
- `bench 10`, `bench 100`, `bench 10` hintereinander: 0 Panic, 0 Leak
- `sleep 1000` → echte 1000 ms
- `piper`, `pipe-test`, `sub 0 100` → alle grün
- `ping 10.0.2.2` → 3/3 Replies (ICMP-Roundtrip)
- `r3ping 10.0.2.2` → Ring-3-Syscall, EXIT code=3
- `udpsend 10.0.2.2 53 hello` → TX-Pfad bestätigt

## Bekannte Einschränkungen

1. **Shell-Echo-Glitch**: bei Async-Ausgabe ([TASK]-Start-Logs) kollidiert
   User-Input mit Log-Zeilen. Kosmetisch.
2. **~100-Page-Sockel nach `bench 100`**: einmalig, nicht kumulativ,
   Baseline 2147 → 2244, bleibt konstant. Vermutlich bench-Setup-Puffer.
   Nicht kritisch, aber offen.
3. **UDP-RX via `hostfwd`**: QEMU-SLIRP-Limitation. UDP-hostfwd braucht
   aktiven Guest-Flow, sonst wird Host→Guest verworfen. TCP wird das in
   A11.5 zuverlässig umgehen.
4. **IO-APIC**: erkannt, MMIO gemappt (`[IOAPIC] id=0 version=20 pins=24`),
   `route/mask/eoi` vorhanden, aber nicht aktiv. Keyboard braucht's nicht
   (COM1-Polling). virtio-blk/net pollen. Wird erst für SMP/echte IRQs relevant.

## Letzte Meilensteine (chronologisch)

| Tag | Inhalt |
|-----|--------|
| `v0.9-b1-final` | B1-Syscall-Return gefixt (iretq statt sysretq) |
| `v0.9-a10c-2-clean` | IO-APIC-Probe (Phase 1), Keyboard-IRQ als irrelevant erkannt |
| `v0.9-a8b-3` | Syscall-Suspend komplett (yield + exit + spawn-frame) |
| `v0.9-a11-1` | virtio-net Driver (PCI, MAC, Queues) |
| `v0.9-a11-2` | ARP + TX/RX (erster Netzwerk-Roundtrip) |
| `v0.9-a11-3a` | IPv4 + ICMP (ping grün) |
| `v0.9-a11-3b` | Ring-3-Ping (Syscall nr=200) |
| `v0.9-a11-4b` | UDP + Socket-Layer (Syscalls 202-205) |

## B1 — Post-Mortem

**Symptom:** `[SCHED] CORRUPT cs pid=N cs=0x2b` bei Ring-3-Tasks unter Last.

**Ursache:** `syscall_entry` benutzte `sysretq`. Berechnet CS/SS aus
`STAR[63:48]`: CS=+16|3, SS=+8|3. NEXUS-GDT: User-CS bei Index 3, User-DS
bei Index 4. Kein STAR-Wert macht beides richtig:
- STAR=0x18 → CS=0x2b (TSS!), SS=0x23 (korrekt) → Guard-Panic
- STAR=0x08 → CS=0x1b (korrekt), SS=0x13 (Kernel-Data!) → #GP err=0x10

**Fix:** `syscall_entry` baut expliziten iretq-Frame (CS=0x1b, SS=0x23).
`SYSRETQ` entfernt. `SYSCALL` (Eintritt) unverändert — STAR[47:32]=0x08.

**Lesson:** `[SAVE-BAD]`-Logging am Frame-Save war in 5 Minuten ergiebiger
als 5 Stunden Code-Spekulation. Bei Ring-3-Crashes: erst Frame dumpen,
dann theoretisieren.

## Nächster Meilenstein
**A11.5** — TCP (3-Way-Handshake, HTTP-GET, FIN-Teardown).
Scope: MVP ohne Retransmit (QEMU-SLIRP ist lossless). Ziel: `nc -l 8000`
am Host, `httpget 10.0.2.2 8000 /` in NEXUS, Antwort wird ausgegeben.

## A11.4 — UDP + Socket-Layer (Details)

**Was gebaut:**
- `net::udp_open/send/recv/close/drain`
- Syscalls 202-205 (Ring-3-Zugriff)
- Shell: `udpsend <ip> <port> <text>`, `udprecv <port> [timeout]`
- Makefile: `hostfwd=udp::5000-10.0.2.15:5000`

**Was getestet:**
- ✅ TX: `udpsend 10.0.2.2 53 hello-nexus` → `[NET ] UDP TX -> ...`
- ⚠️ RX (Host→Guest via hostfwd): Timeout — QEMU-SLIRP-Limitation.
  Beweis dass RX-Pfad grundsätzlich geht: `ping 10.0.2.2` empfängt
  Replies über identischen `net_poll`-Pfad.

**Syscall-Referenz:**
- nr=200 `net_ping(dst_ip_be, count, timeout_ms)` → recv_count
- nr=201 `net_mac(user_ptr6)` → 0
- nr=202 `udp_open(port)` → idx (0..7) oder -1
- nr=203 `udp_send(idx, dst_ip_be, dst_port, user_ptr, len)` → 0 oder neg
- nr=204 `udp_recv(idx, user_buf, cap, timeout_ms)` → n oder neg
- nr=205 `udp_close(idx)` → 0
