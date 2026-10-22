// kernel/dev/virtio_net.cpp — A11.1: virtio-net legacy Driver (init only)
#include "kernel/dev/virtio_net.h"
#include "kernel/dev/pci.h"
#include "kernel/mm/mm.h"

namespace virtio_net {

static constexpr u16 R_HOST_FEATURES = 0x00;
static constexpr u16 R_GUEST_FEATURES= 0x04;
static constexpr u16 R_QUEUE_ADDR    = 0x08;
static constexpr u16 R_QUEUE_SIZE    = 0x0C;
static constexpr u16 R_QUEUE_SELECT  = 0x0E;
static constexpr u16 R_QUEUE_NOTIFY  = 0x10;
static constexpr u16 R_DEV_STATUS    = 0x12;
static constexpr u16 R_ISR_STATUS    = 0x13;
static constexpr u16 R_DEVICE_CFG    = 0x14;

static constexpr u8 ST_ACK       = 1;
static constexpr u8 ST_DRIVER    = 2;
static constexpr u8 ST_DRIVER_OK = 4;

static constexpr u32 VIRTIO_NET_F_MAC = 1u << 5;
static constexpr u32 VIRTIO_NET_F_STATUS = 1u << 16;
static constexpr u16 Q_RX = 0;
static constexpr u16 Q_TX = 1;

struct Desc { u64 addr; u32 len; u16 flags; u16 next; };
struct UsedElem { u32 id; u32 len; };

struct Queue {
    u16 size;
    u8* mem;
    u64 phys;
    volatile Desc* desc;
    volatile u16* avail_idx;
    volatile u16* avail_ring;
    volatile u16* used_idx;
    volatile UsedElem* used_ring;
    volatile u16 last_used;
};

static Queue rx_q {};
static Queue tx_q {};

static constexpr u32 N_RX = 8;
static constexpr u32 RXBUF_SZ = 2048;

static u8*  rx_bufs = nullptr;
static u64  rx_bufs_phys = 0;
static u8*  tx_scratch = nullptr;
static u64  tx_scratch_phys = 0;

static u64  stat_rx_pkts = 0;
static u64  stat_tx_pkts = 0;
static u64  stat_rx_bytes = 0;
static u64  stat_tx_bytes = 0;

// Forward-Declaration (init ruft fill_rx vor Definition)
static void fill_rx();

static u16 io_base = 0;
static u8  mac_addr[6] = {0};
static bool ready = false;

static int queue_setup(Queue* q, u16 qidx, const char* label) {
    hal::outw(io_base + R_QUEUE_SELECT, qidx);
    u16 qsz = hal::inw(io_base + R_QUEUE_SIZE);
    if (qsz == 0) {
        hal::pf("[NET ] %s qsize=0, abort\n", label);
        return -1;
    }
    q->size = qsz;

    u32 desc_bytes  = qsz * 16;
    u32 avail_off   = desc_bytes;
    u32 used_off    = (avail_off + 6 + 2 * qsz + 4095) & ~4095u;
    u32 used_bytes  = 6 + 8 * qsz;
    u32 total_bytes = used_off + used_bytes;
    u32 pages       = (total_bytes + 4095) / 4096;

    u64 phys = mem::pmm::alloc_n(pages);
    if (!phys) {
        hal::pf("[NET ] %s pmm alloc fail\n", label);
        return -1;
    }
    q->mem = (u8*)mem::p2v(phys);
    q->phys = phys;
    for (u32 i = 0; i < pages * 4096; i++) q->mem[i] = 0;

    q->desc       = (volatile Desc*)(q->mem + 0);
    q->avail_idx  = (volatile u16*)(q->mem + avail_off + 2);
    q->avail_ring = (volatile u16*)(q->mem + avail_off + 4);
    q->used_idx   = (volatile u16*)(q->mem + used_off + 2);
    q->used_ring  = (volatile UsedElem*)(q->mem + used_off + 4);
    q->last_used  = 0;

    hal::pf("[NET ] %s q: qsz=%u desc=%u avail=%u used=%u pages=%u\n",
            label, (u32)qsz, desc_bytes, avail_off, used_off, pages);
    hal::pf("[NET ] %s q_mem virt=%p phys=%p\n",
            label, (u64)q->mem, q->phys);

    hal::outl(io_base + R_QUEUE_ADDR, (u32)(q->phys >> 12));
    return 0;
}

int init() {
    hal::ps("[NET ] scanning for virtio-net legacy...\n");
    for (u32 bus = 0; bus < 256; bus++) {
        for (u32 dev = 0; dev < 32; dev++) {
            for (u32 fn = 0; fn < 8; fn++) {
                u16 vendor = pci::cfg_read16((u8)bus, (u8)dev, (u8)fn, 0x00);
                if (vendor != 0x1AF4) continue;
                u16 device = pci::cfg_read16((u8)bus, (u8)dev, (u8)fn, 0x02);
                if (device < 0x1000 || device > 0x103F) continue;
                if (device != 0x1000) continue;

                hal::pf("[NET ] virtio-net device %x:%x at %u:%u.%u\n",
                        (u32)vendor, (u32)device, bus, dev, fn);

                u32 oldcmd = pci::cfg_read32((u8)bus, (u8)dev, (u8)fn, 0x04);
                pci::cfg_write32((u8)bus, (u8)dev, (u8)fn, 0x04, oldcmd | 0x5);

                u32 bar0 = pci::cfg_read32((u8)bus, (u8)dev, (u8)fn, 0x10);
                io_base = (u16)(bar0 & ~0x3);
                hal::pf("[NET ] BAR0=%x io_base=%x\n", (u32)bar0, (u32)io_base);

                hal::outb(io_base + R_DEV_STATUS, 0);
                for (int i = 0; i < 1000; i++)
                    if (hal::inb(io_base + R_DEV_STATUS) == 0) break;

                hal::outb(io_base + R_DEV_STATUS, ST_ACK);
                hal::outb(io_base + R_DEV_STATUS, ST_ACK | ST_DRIVER);

                u32 hf = hal::inl(io_base + R_HOST_FEATURES);
                hal::pf("[NET ] host features=%x\n", hf);
                if (!(hf & VIRTIO_NET_F_MAC)) {
                    hal::ps("[NET ] VIRTIO_NET_F_MAC not supported\n");
                    return -2;
                }
                hal::outl(io_base + R_GUEST_FEATURES, VIRTIO_NET_F_MAC | VIRTIO_NET_F_STATUS);

                u32 w0 = hal::inl(io_base + R_DEVICE_CFG);
                u16 w1 = hal::inw(io_base + R_DEVICE_CFG + 4);
                mac_addr[0] = (u8)(w0 & 0xFF);
                mac_addr[1] = (u8)((w0 >> 8) & 0xFF);
                mac_addr[2] = (u8)((w0 >> 16) & 0xFF);
                mac_addr[3] = (u8)((w0 >> 24) & 0xFF);
                mac_addr[4] = (u8)(w1 & 0xFF);
                mac_addr[5] = (u8)((w1 >> 8) & 0xFF);
                hal::pf("[NET ] MAC = %x:%x:%x:%x:%x:%x\n",
                        (u32)mac_addr[0], (u32)mac_addr[1],
                        (u32)mac_addr[2], (u32)mac_addr[3],
                        (u32)mac_addr[4], (u32)mac_addr[5]);
                {
                    u16 ls = hal::inw(io_base + R_DEVICE_CFG + 6);
                    hal::pf("[NET ] link_status=%x%s\n", (u32)ls,
                            (ls & 1) ? " UP" : " DOWN");
                }

                if (queue_setup(&rx_q, Q_RX, "rx") < 0) return -3;
                if (queue_setup(&tx_q, Q_TX, "tx") < 0) return -4;

                hal::outb(io_base + R_DEV_STATUS, ST_ACK | ST_DRIVER | ST_DRIVER_OK);

                u8 status = hal::inb(io_base + R_DEV_STATUS);
                hal::pf("[NET ] verify: dev_status=%x\n", (u32)status);

                // tx_scratch als physischer Buffer
                {
                    u64 tp = mem::pmm::alloc_n(1);
                    if (!tp) { hal::ps("[NET ] tx scratch alloc fail\n"); return -5; }
                    tx_scratch = (u8*)mem::p2v(tp);
                    for (int i = 0; i < 4096; i++) tx_scratch[i] = 0;
                    tx_scratch_phys = tp;
                    hal::pf("[NET ] tx_scratch virt=%p phys=%p\n",
                            (u64)tx_scratch, tx_scratch_phys);
                }

                ready = true;
                fill_rx();
                hal::ps("[NET ] ready (MAC read, RX prefilled, TX ready)\n");
                return 0;
            }
        }
    }
    hal::ps("[NET ] no virtio-net device found (QEMU flag missing?)\n");
    return -1;
}

bool is_ready() { return ready; }
void get_mac(u8 mac[6]) {
    for (int i = 0; i < 6; i++) mac[i] = mac_addr[i];
}
u32 rx_qsize() { return rx_q.size; }
u32 tx_qsize() { return tx_q.size; }


// ── TX / RX / ARP ──────────────────────────────────────────────

static void fill_rx() {
    if (!rx_bufs) {
        u64 p = mem::pmm::alloc_n((N_RX * RXBUF_SZ + 4095) / 4096);
        if (!p) {
            hal::ps("[NET ] fill_rx: pmm alloc fail\n");
            return;
        }
        rx_bufs = (u8*)mem::p2v(p);
        rx_bufs_phys = p;
        for (u32 i = 0; i < N_RX * RXBUF_SZ; i++) rx_bufs[i] = 0;
    }
    for (u32 i = 0; i < N_RX; i++) {
        rx_q.desc[i].addr  = rx_bufs_phys + i * RXBUF_SZ;
        rx_q.desc[i].len   = RXBUF_SZ;
        rx_q.desc[i].flags = 2;   // F_WRITE (device writes to us)
        rx_q.desc[i].next  = 0;
        rx_q.avail_ring[i] = (u16)i;
    }
    __sync_synchronize();
    *rx_q.avail_idx = N_RX;
    __sync_synchronize();
    hal::outw(io_base + R_QUEUE_NOTIFY, Q_RX);
    hal::pf("[NET ] rx queue pre-filled (%u buffers x %u bytes)\n",
            N_RX, RXBUF_SZ);
}

int net_tx(const u8* frame, u32 len) {
    if (!ready) return -1;
    if (len > 2048 - 10) return -1;

    if (!tx_scratch || !tx_scratch_phys) return -1;

    // virtio-net-Header (10 Bytes, alle 0 fuer plain frame)
    for (int i = 0; i < 10; i++) tx_scratch[i] = 0;
    for (u32 i = 0; i < len; i++) tx_scratch[10 + i] = frame[i];
    u32 total = 10 + len;

    tx_q.desc[0].addr  = tx_scratch_phys;
    tx_q.desc[0].len   = total;
    tx_q.desc[0].flags = 0;
    tx_q.desc[0].next  = 0;
    __sync_synchronize();

    u16 slot = *tx_q.avail_idx % tx_q.size;
    tx_q.avail_ring[slot] = 0;
    __sync_synchronize();
    *tx_q.avail_idx = (u16)(*tx_q.avail_idx + 1);
    __sync_synchronize();

    hal::outw(io_base + R_QUEUE_NOTIFY, Q_TX);

    u32 spins = 0;
    while (tx_q.last_used == *tx_q.used_idx) {
        for (int i = 0; i < 100; i++) __asm__ volatile("" ::: "memory");
        if (++spins > 500000) {
            hal::pf("[NET ] tx timeout (used_idx=%u last=%u)\n",
                    (u32)*tx_q.used_idx, (u32)tx_q.last_used);
            return -2;
        }
    }
    tx_q.last_used = (u16)(tx_q.last_used + 1);
    stat_tx_pkts++;
    stat_tx_bytes += len;
    return 0;
}

int net_poll(u8* out, u32 cap) {
    if (!ready) return -1;
    if (*rx_q.used_idx == rx_q.last_used) return 0;

    __sync_synchronize();
    u32 slot = rx_q.last_used % rx_q.size;
    u32 id = rx_q.used_ring[slot].id;
    u32 len = rx_q.used_ring[slot].len;
    rx_q.last_used = (u16)(rx_q.last_used + 1);

    if (id >= N_RX || len > RXBUF_SZ || len < 10) {
        // kaputt, aber Buffer zurueckgeben
        rx_q.avail_ring[*rx_q.avail_idx % rx_q.size] = (u16)(id < N_RX ? id : 0);
        __sync_synchronize();
        *rx_q.avail_idx = (u16)(*rx_q.avail_idx + 1);
        __sync_synchronize();
        hal::outw(io_base + R_QUEUE_NOTIFY, Q_RX);
        return -1;
    }

    u32 frame_len = len - 10;
    if (frame_len > cap) frame_len = cap;
    u8* src = rx_bufs + id * RXBUF_SZ + 10;
    for (u32 i = 0; i < frame_len; i++) out[i] = src[i];

    // Buffer wiederverwenden
    rx_q.avail_ring[*rx_q.avail_idx % rx_q.size] = (u16)id;
    __sync_synchronize();
    *rx_q.avail_idx = (u16)(*rx_q.avail_idx + 1);
    __sync_synchronize();
    hal::outw(io_base + R_QUEUE_NOTIFY, Q_RX);

    stat_rx_pkts++;
    stat_rx_bytes += frame_len;
    return (int)frame_len;
}

// Baut einen ARP-Request fuer target_ip von guest_ip (10.0.2.15 hart kodiert).
// Sendet, pollt bis zu ~500ms auf Reply. Bei Erfolg: out_mac gefuellt, return 0.
int arp_resolve(const u8 ip[4], u8 out_mac[6]) {
    if (!ready) return -1;

    u8 frame[42];
    // Ethernet
    for (int i = 0; i < 6; i++) frame[i] = 0xFF;   // broadcast
    for (int i = 0; i < 6; i++) frame[6+i] = mac_addr[i];
    frame[12] = 0x08; frame[13] = 0x06;            // ARP

    // ARP-Payload
    frame[14] = 0; frame[15] = 1;                  // htype = Ethernet
    frame[16] = 0x08; frame[17] = 0x00;            // ptype = IPv4
    frame[18] = 6;  frame[19] = 4;                 // hlen=6, plen=4
    frame[20] = 0;  frame[21] = 1;                 // oper = request
    for (int i = 0; i < 6; i++) frame[22+i] = mac_addr[i];
    // sender IP = 10.0.2.15 (Guest-Standard in QEMU user-net)
    frame[28] = 10; frame[29] = 0; frame[30] = 2; frame[31] = 15;
    for (int i = 0; i < 6; i++) frame[32+i] = 0;   // target MAC unbekannt
    frame[38] = ip[0]; frame[39] = ip[1]; frame[40] = ip[2]; frame[41] = ip[3];

    int tr = net_tx(frame, 42);
    if (tr != 0) {
        hal::pf("[NET ] ARP request tx fail (err %d)\n", tr);
        return -2;
    }
    hal::pf("[NET ] TX ARP request for %u.%u.%u.%u\n",
            ip[0], ip[1], ip[2], ip[3]);

    // Poll auf Reply (~500ms)
    u8 rxbuf[1500];
    for (u32 spins = 0; spins < 500; spins++) {
        int n = net_poll(rxbuf, sizeof(rxbuf));
        if (n > 0) {
            if (n >= 42 && rxbuf[12] == 0x08 && rxbuf[13] == 0x06 &&
                rxbuf[20] == 0 && rxbuf[21] == 2) {
                // ARP reply. Pruefe, ob sender IP == target IP
                if (rxbuf[28] == ip[0] && rxbuf[29] == ip[1] &&
                    rxbuf[30] == ip[2] && rxbuf[31] == ip[3]) {
                    for (int i = 0; i < 6; i++) out_mac[i] = rxbuf[22+i];
                    hal::pf("[NET ] RX ARP reply (%d bytes)\n", n);
                    return 0;
                }
            }
        }
        for (int i = 0; i < 10000; i++) __asm__ volatile("" ::: "memory");
    }
    hal::pf("[NET ] ARP timeout after 500ms\n");
    return -3;
}

void stats(u64* rx_pkts, u64* tx_pkts, u64* rx_bytes, u64* tx_bytes) {
    if (rx_pkts)   *rx_pkts   = stat_rx_pkts;
    if (tx_pkts)   *tx_pkts   = stat_tx_pkts;
    if (rx_bytes)  *rx_bytes  = stat_rx_bytes;
    if (tx_bytes)  *tx_bytes  = stat_tx_bytes;
}

}  // namespace virtio_net
