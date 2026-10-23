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

## A11.5 — TCP + HTTP-Client (MVP) (2025-10-23)
**Was gebaut:**
- `kernel/dev/tcp.cpp` — TCP-Client mit State-Machine
  (CLOSED → SYN_SENT → ESTABLISHED → FIN_WAIT)
- `net::tcp_open/connect/send/recv/close`
- `net::ip_drain()` — zentraler IP-Dispatch für UDP+TCP
- Syscalls 210-213 (tcp_open/connect/send/recv)
- Shell: `httpget <ip> <port> <path>`

**Was getestet:**
- ✅ End-to-End HTTP-Client: `httpget 10.0.2.2 8000 /`
  - SYN → SYN/ACK → ACK (Handshake)
  - GET / HTTP/1.1 raus, 200 OK + Body rein
  - FIN-Teardown sauber
- Host-Python-Server sah: `GET / HTTP/1.1\r\nHost: 10.0.2.2\r\nConnection: close`

**Nicht implementiert (MVP-Scope):**
- Kein Retransmit (QEMU-SLIRP ist lossless)
- Kein Window-Advertising (Window im Header fix 1460)
- Kein Sliding-Window / Congestion-Control
- Keine Out-of-Order-Reassembly (setzt in-order voraus)

**Offenes Detail:** Wenn Server Daten UND FIN in schneller Folge sendet,
verarbeitet `ip_drain` beide im selben Poll-Durchlauf. Aktuell OK, weil
`tcp_recv` erst Buffer leert, dann EOF gibt. Bei größeren Responses
mit Segment-Reordering könnte das Probleme machen — Fix wäre ein
kleiner Timer/Reassembly-Mechanismus.

## A11 komplett — Netzwerk-Stack L2→L7-Client

| Layer | Status | Tag |
|-------|--------|-----|
| virtio-net L2 | ✅ | v0.9-a11-1 |
| ARP + TX/RX | ✅ | v0.9-a11-2 |
| IPv4 + ICMP | ✅ | v0.9-a11-3a |
| Ring-3-Ping | ✅ | v0.9-a11-3b |
| UDP + Socket | ✅ | v0.9-a11-4b |
| TCP + HTTP | ✅ | v0.9-a11-5 |

**Was jetzt möglich ist:** NEXUS kann als Client mit dem Internet reden
(via QEMU-SLIRP), HTTP-Anfragen stellen, Antworten ausgeben.

## A11.6/7 — DNS + Gateway-Routing (2025-10-23)
**Was gebaut:**
- `kernel/dev/dns.cpp` — minimaler DNS-Resolver (A-Record only)
- `net::dns_resolve(hostname, out_ip)` via QEMU-SLIRP DNS (10.0.2.3)
- Syscall nr=220 (dns_resolve aus Ring-3)
- Shell: `dns <hostname>`, `httpget <host>` (Host kann IP ODER Hostname sein)
- `ip_send` Gateway-Routing: externe Ziele → ARP fuers Gateway (10.0.2.2),
  IP-Header behaelt Ziel-IP

**Was getestet:**
- ✅ `dns example.com` → `104.20.23.154`
- ✅ `httpget example.com 80 /` → echte HTTP/1.1-200-Antwort von Cloudflare
- ✅ Lokales HTTP gegen Python-Server weiterhin OK

**Erster echter Internet-Zugriff aus NEXUS.**

**Bekannte Limitierung (offen):**
- HTTP-Body bei `Transfer-Encoding: chunked` wird nicht dekodiert.
  Response-Header kommen sauber, Body bleibt aus. Fix waere Chunked-Parser
  in `cmd_httpget`. Fuer "HTTP-Download" (mit Content-Length) ist der
  aktuelle Code ausreichend.
- TCP-Segment-Reordering + FIN-im-selben-Poll-Durchlauf: funktioniert
  aktuell, aber bei sehr schnellen Servern koennte das letzte Datensegment
  vor FIN verloren gehen. Fix: FIN-Handling verzoegern bis Buffer leer.

## B1.2 — Persistenz gehaertet (2025-10-23)
**Design-Umstellung:** Payload lebt jetzt in eigenen LLRB-Records,
kein RAM-Pool mehr.

- `NS_MSG`  (0x09) — die Nachricht: key=`NS_MSG|seq<<8|ch`, a=0, b=len
- `NS_MSGP` (0x0A) — Payload-Chunk: key=`NS_MSGP|seq<<8|ch<<4|ci`,
  a=bytes[0..7], b=bytes[8..15]

**Key-Layout-Praezisierung:**
- seq belegt 48 Bit (nicht 56 wie urspruenglich gedacht)
- Maske fuer seq-Extraktion: `0x0000FFFFFFFFFFFF`
- MSG: bits 63..56 = NS, bits 55..8 = seq, bits 7..0 = ch
- MSGP: bits 63..56 = NS, bits 55..8 = seq, bits 7..4 = ch, bits 3..0 = ci

**Filter-Fixes (4 Stellen in persist.cpp):**
- `collect_visit` (Save): `&& r->type != 9` ergaenzt
- `backup_visit` (Rollback): `&& r->type != 9` ergaenzt
- `load()` (Snapshot): `type > 8` -> `type > 9`
- `ckpt_restore_data`: `type > 8` -> `type > 9`

**Was getestet:**
- ✅ In-Session: `msgsend` + `msgrecv` rekonstruiert Payload korrekt
- ✅ WAL-Persistenz: 3 Messages gesendet, kill -9, Reboot -> alle 3 via WAL-Replay
- ✅ Snapshot-Persistenz: `checkpoint` + kill -9 + Reboot -> Message via Snapshot
- ✅ Multi-Channel, FIFO, Consume (nach recv ist Record weg)

**Bekannte Limitierung (nicht blockierend):**
- Bei wiederholten Snapshot+WAL-Zyklen koennen MSGP-Chunks dupliziert
  werden (ueberschreiben sich beim Reconstruct, aber Index waechst).
  Fix waere ein dediziertes `idx::put` mit "already exists = no-op" oder
  Cleanup beim Delete.
- 16-byte-Chunks sind ineffizient fuer kurze Messages (1 MSGP pro 16
  Bytes = 6 Records fuer 100-byte-Message). Bei Bedarf: MSGP auf 48
  Bytes erweitern (a+b+mehr Felder) — braucht Rec-Format-Erweiterung.

**Naechster Schritt:** B1.3 (Syscalls 230-232), dann B1.4 (Pipes auf MSG).

## A-final-1 — HTTP Robustheit (2025-10-23)
**Was gebaut:**
- `kernel/dev/http.cpp` + `http.h` — eigene HTTP-Schicht
  - Header/Body-Trennung via `\r\n\r\n`
  - Chunked-Transfer-Encoding-Decoder
  - Content-Length-Modus (wenn kein Chunked)
- `tcp_handle_frame`: FIN-Branch puffert jetzt Payload vor `state=CLOSED`
- `rx_buf[4][512]` → `rx_buf[8][1460]` (8 × MSS, ~11 KB Total)
- `rcv_nxt` bei Data korrekt `seq + paylen` (nicht `+ cp`; cp==paylen immer weil 1460 == MSS)
- `cmd_httpget` refactored: nutzt `http_get`, trennt Header-Ausgabe von Body

**Was getestet:**
- ✅ Lokal: `httpget 10.0.2.2 8000 /` → body=1063, komplettes Directory-Listing
- ✅ Cloudflare: `httpget example.com 80 /` → body=577, echtes HTML
  (raw_len=932 → 355 Bytes Chunked-Framing dekodiert)

**Bekannte Limitierung:** Content-Encoding gzip wird nicht dekodiert (Body
bleibt komprimiert, wenn Server `Content-Encoding: gzip` sendet ohne dass
wir `Accept-Encoding` anfordern — meist nicht der Fall bei `Connection: close`).

## A-final-2 — Slab-Trim (2025-10-23)
**Problem:** `bench 100` hinterließ ~100-136 Pages Sockel — Slab-Chunks
wurden allokiert, aber beim Free nicht an PMM zurückgegeben (klassisches
Slab-Verhalten). Bei wiederholten `bench 100` kumulativ.

**Fix:**
- `ChunkHdr` als eigenes struct vor `struct Class`, mit
  `{phys, magic, next, pages, objs_per_chunk, free_objs}` (40 Bytes
  statt 16 — RAM-only, kein Disk-Format betroffen)
- `Class` bekommt `ChunkHdr* chunks` + `u32 n_chunks`
- `grow()` trackt `free_objs = objs_per_chunk` initial
- `find_chunk(c, slot)` — sucht Chunk für eine Slot-Adresse
- `alloc`: `free_objs--` beim Pop
- `free`: `free_objs++` beim Push
- `slab::trim()`: leere Chunks aus Liste, `free_list` bereinigen,
  `pmm::free_n(phys, pages)`, `[SLAB] trim: N pages freed`
- Shell: `slab-trim` — manuell aufrufbar

**Verifiziert:**
- bench 100 → mem → used=2255, slab-trim → 120 pages freed → used=2135
- bench 100 → mem → used=2271, slab-trim → 136 pages freed → used=2135
- Nach jedem trim exakt Baseline, kein kumulatives Wachstum
- `slab: 27 objects in use` bleibt konstant (TCB-Tracking korrekt)

**Bekannte Limitierung:** `slab-trim` ist manuell — nicht automatisch
nach reap. Bei Bedarf: nach jedem REAP-Batch oder bei
`used_count > threshold` aufrufen.

## A-final-3 — cat_file streaming (2025-10-23)
**Fix:** `cat`/`open`/`read` laden bei `O_RDONLY` die Datei nicht mehr
komplett in `ram_buf` — Streaming aus der Extent-Kette.

- `fs_read_at(file_key, offset, buf, len)`: liest N Bytes, kein Heap
- `sys_open_kernel`: `ram_buf` nur bei `O_WRONLY`/`O_RDWR` laden
- `sys_read_kernel`: wenn `ram_buf == 0` → `fs_read_at` streaming
- `cmd_cat`: chunked `fs_read_at` direkt auf stdout

**Verifiziert:**
- `gen big 100000` → `cat big` (100 KB Muster) → `mem`:
  `heap: 0 KiB used`, `pmm: used=2143` unverändert
- Vorher: `heap: ~100 KiB`, `pmm: +25 Pages` Sockel durch ram_buf

**Bekannte Limitierung:** `sys_write` mit großen Datenströmen
(`write`-Pfad) nutzt weiter `ram_buf`. Schreiben > 1 MB ist limitiert.
Fix wäre `fs_write_at` mit Extent-Wachstum — eigene Session (A-S7b).
