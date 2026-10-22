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
                hal::outl(io_base + R_GUEST_FEATURES, VIRTIO_NET_F_MAC);

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

                if (queue_setup(&rx_q, Q_RX, "rx") < 0) return -3;
                if (queue_setup(&tx_q, Q_TX, "tx") < 0) return -4;

                hal::outb(io_base + R_DEV_STATUS, ST_ACK | ST_DRIVER | ST_DRIVER_OK);

                u8 status = hal::inb(io_base + R_DEV_STATUS);
                hal::pf("[NET ] verify: dev_status=%x\n", (u32)status);

                ready = true;
                hal::ps("[NET ] ready (init only, no packets yet)\n");
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

}  // namespace virtio_net
