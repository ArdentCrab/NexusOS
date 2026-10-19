// kernel/dev/virtio_blk.cpp
#include "kernel/dev/virtio_blk.h"
#include "kernel/dev/pci.h"
#include "kernel/mm/mm.h"

namespace virtio_blk {

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

static u16 qsize = 0;   // vom Device gesetzt (typisch 256)
static constexpr u16 F_NEXT  = 1;
static constexpr u16 F_WRITE = 2;

struct Desc { u64 addr; u32 len; u16 flags; u16 next; };
struct UsedElem { u32 id; u32 len; };

static u8* q_mem = nullptr;
static u64 q_phys = 0;
static volatile Desc* q_desc = nullptr;
static volatile u16* q_avail_idx = nullptr;
static volatile u16* q_avail_ring = nullptr;
static volatile u16* q_used_idx = nullptr;
static volatile UsedElem* q_used_ring = nullptr;
static volatile u16 q_last_used = 0;

static u16 io_base = 0;
static u64 cap_sectors = 0;
static bool ready = false;

static u8* req_hdr     = nullptr;
static u8* req_data    = nullptr;
static u8* req_status  = nullptr;
static u64 req_hdr_pa  = 0;
static u64 req_data_pa = 0;
static u64 req_status_pa = 0;

static void queue_init() {
    // Layout nach VirtIO 0.9.5:
    //   desc:    16 * qsz bytes, aligned 16
    //   avail:   6 + 2*qsz, aligned 2, direkt hinter desc
    //   used:    6 + 8*qsz, aligned 4096
    u32 desc_bytes  = qsize * 16;
    u32 avail_off   = desc_bytes;
    u32 used_off    = (avail_off + 6 + 2 * qsize + 4095) & ~4095u;
    u32 used_bytes  = 6 + 8 * qsize;
    u32 total_bytes = used_off + used_bytes;
    u32 pages       = (total_bytes + 4095) / 4096;

    u64 phys = mem::pmm::alloc_n(pages);
    q_mem = (u8*)mem::p2v(phys);
    q_phys = phys;
    for (u32 i = 0; i < pages * 4096; i++) q_mem[i] = 0;

    q_desc       = (volatile Desc*)(q_mem + 0);
    q_avail_idx  = (volatile u16*)(q_mem + avail_off + 2);
    q_avail_ring = (volatile u16*)(q_mem + avail_off + 4);
    q_used_idx   = (volatile u16*)(q_mem + used_off + 2);
    q_used_ring  = (volatile UsedElem*)(q_mem + used_off + 4);
    q_last_used  = 0;

    hal::pf("[VIO ] q layout: qsz=%u desc=%u avail=%u used=%u pages=%u\n",
            (u32)qsize, desc_bytes, avail_off, used_off, pages);
    hal::pf("[VIO ] q_mem virt=%p phys=%p\n", (u64)q_mem, q_phys);

    // Request-Buffer
    u64 rp = mem::pmm::alloc_n(3);
    u8* rb = (u8*)mem::p2v(rp);
    for (int i = 0; i < 3 * 4096; i++) rb[i] = 0;
    req_hdr    = rb;
    req_data   = rb + 4096;
    req_status = rb + 8192;
    req_hdr_pa    = rp;
    req_data_pa   = rp + 4096;
    req_status_pa = rp + 8192;
}

static int blk_request_locked(u32 type, u64 sector, u32 n_sectors, void* buf);

static Spinlock blk_lock {0};

static int blk_request(u32 type, u64 sector, u32 n_sectors, void* buf) {
    // IRQ-safe: waehrend blk_lock gehalten wird, darf Timer-IRQ NICHT
    // preempten. Sonst kann ein anderer Task (der gerade einen Syscall
    // macht, IF=0) auf blk_lock warten und wir deadlocken:
    //   Shell haelt blk_lock, preempted -> Task X wartet auf blk_lock
    //   in syscall (IF=0). Task X kann Shell nicht preempten, Shell kann
    //   Lock nicht freigeben. Deadlock.
    u64 rflags = spin_lock_irqsave(&blk_lock);
    int r = blk_request_locked(type, sector, n_sectors, buf);
    spin_unlock_irqrestore(&blk_lock, rflags);
    return r;
}

static int blk_request_locked(u32 type, u64 sector, u32 n_sectors, void* buf) {
    if (!ready) return -1;

    *(u32*)(req_hdr + 0) = type;
    *(u32*)(req_hdr + 4) = 0;
    *(u64*)(req_hdr + 8) = sector;

    u32 nbytes = n_sectors * 512;
    if (type == 1 && buf) {
        u8* src = (u8*)buf;
        for (u32 i = 0; i < nbytes && i < 512; i++) req_data[i] = src[i];
    }
    req_status[0] = 0xFF;

    q_desc[0].addr  = req_hdr_pa;
    q_desc[0].len   = 16;
    q_desc[0].flags = F_NEXT;
    q_desc[0].next  = 1;

    q_desc[1].addr  = req_data_pa;
    q_desc[1].len   = nbytes;
    q_desc[1].flags = F_NEXT | (type == 0 ? F_WRITE : 0);
    q_desc[1].next  = 2;

    q_desc[2].addr  = req_status_pa;
    q_desc[2].len   = 1;
    q_desc[2].flags = F_WRITE;
    q_desc[2].next  = 0;

    // Speicherbarriere: sicherstellen dass desc-Writes sichtbar sind,
    // BEVOR avail_idx steigt
    __sync_synchronize();

    u16 slot = *q_avail_idx % qsize;
    q_avail_ring[slot] = 0;
    __sync_synchronize();
    *q_avail_idx = (u16)(*q_avail_idx + 1);
    __sync_synchronize();

    // Notify erst nach allen Writes
    hal::outw(io_base + R_QUEUE_NOTIFY, 0);

    u32 spins = 0;
    while (q_last_used == *q_used_idx) {
        for (int i = 0; i < 100; i++) __asm__ volatile("" ::: "memory");
        if (++spins > 5000000) {
            // Diagnose: lies den aktuellen Zustand zurueck
            u32 qaddr = hal::inl(io_base + R_QUEUE_ADDR);
            u8  status = hal::inb(io_base + R_DEV_STATUS);
            u8  isr = hal::inb(io_base + R_ISR_STATUS);
            hal::pf("[VIO ] TIMEOUT after 50M spins\n");
            hal::pf("[VIO ]   used_idx=%u last_used=%u\n",
                    (u32)*q_used_idx, (u32)q_last_used);
            hal::pf("[VIO ]   q_phys>>12=%x  queue_addr_reg=%x\n",
                    (u32)(q_phys >> 12), (u32)qaddr);
            hal::pf("[VIO ]   dev_status=%x  isr=%x  req_status=%x\n",
                    (u32)status, (u32)isr, (u32)req_status[0]);
            return -2;
        }
    }
    q_last_used = (u16)(q_last_used + 1);

    if (req_status[0] != 0) {
        hal::pf("[VIO ] device status=%p\n", (u32)req_status[0]);
        return -3;
    }

    if (type == 0 && buf) {
        u8* dst = (u8*)buf;
        for (u32 i = 0; i < nbytes && i < 512; i++) dst[i] = req_data[i];
    }
    return 0;
}

int init() {
    hal::ps("[VIO ] scanning for virtio-blk legacy...\n");
    for (u32 bus = 0; bus < 256; bus++) {
        for (u32 dev = 0; dev < 32; dev++) {
            for (u32 fn = 0; fn < 8; fn++) {
                u16 vendor = pci::cfg_read16((u8)bus, (u8)dev, (u8)fn, 0x00);
                if (vendor != 0x1AF4) continue;
                u16 device = pci::cfg_read16((u8)bus, (u8)dev, (u8)fn, 0x02);
                hal::pf("[VIO ] virtio device %x:%x at %u:%u.%u\n",
                        (u32)vendor, (u32)device, bus, dev, fn);
                if (device < 0x1000 || device > 0x103F) {
                    hal::ps("[VIO ] not legacy (modern?) — skip\n");
                    continue;
                }
                if (device != 0x1001) {
                    hal::pf("[VIO ] not blk (id %x)\n", (u32)device);
                    continue;
                }

                hal::pf("[VIO ] virtio-blk legacy at %u:%u.%u\n", bus, dev, fn);

                u32 oldcmd = pci::cfg_read32((u8)bus, (u8)dev, (u8)fn, 0x04);
                u32 newcmd = oldcmd | 0x00000005;
                pci::cfg_write32((u8)bus, (u8)dev, (u8)fn, 0x04, newcmd);

                u32 bar0 = pci::cfg_read32((u8)bus, (u8)dev, (u8)fn, 0x10);
                io_base = (u16)(bar0 & ~0x3);
                hal::pf("[VIO ] BAR0=%x io_base=%x\n", (u32)bar0, (u32)io_base);

                hal::outb(io_base + R_DEV_STATUS, 0);
                for (int i = 0; i < 1000; i++)
                    if (hal::inb(io_base + R_DEV_STATUS) == 0) break;

                hal::outb(io_base + R_DEV_STATUS, ST_ACK);
                hal::outb(io_base + R_DEV_STATUS, ST_ACK | ST_DRIVER);

                u32 features = hal::inl(io_base + R_HOST_FEATURES);
                hal::pf("[VIO ] host features=%x\n", features);
                hal::outl(io_base + R_GUEST_FEATURES, 0);

                hal::outw(io_base + R_QUEUE_SELECT, 0);
                u16 qsz = hal::inw(io_base + R_QUEUE_SIZE);
                hal::pf("[VIO ] queue0 size=%u\n", (u32)qsz);
                if (qsz == 0) return -2;
                qsize = qsz;

                queue_init();

                // Queue-Adresse (Legacy: phys >> 12 als 32-bit Wert)
                hal::outl(io_base + R_QUEUE_ADDR, (u32)(q_phys >> 12));
                hal::pf("[VIO ] queue_phys=%p  phys>>12=%x\n",
                        q_phys, (u32)(q_phys >> 12));

                // DRIVER_OK erst NACH Queue-Setup
                hal::outb(io_base + R_DEV_STATUS, ST_ACK | ST_DRIVER | ST_DRIVER_OK);

                // Kapazitaet aus Device-Config
                u32 lo = hal::inl(io_base + R_DEVICE_CFG);
                u32 hi = hal::inl(io_base + R_DEVICE_CFG + 4);
                cap_sectors = ((u64)hi << 32) | lo;
                hal::pf("[VIO ] capacity=%u sectors (%u MiB)\n",
                        (u32)cap_sectors,
                        (u32)(cap_sectors * 512 / 1024 / 1024));

                // Verifikation: Register zuruecklesen
                u32 qaddr_ok = hal::inl(io_base + R_QUEUE_ADDR);
                u8  status_ok = hal::inb(io_base + R_DEV_STATUS);
                hal::pf("[VIO ] verify: qaddr_reg=%x  status=%x\n",
                        (u32)qaddr_ok, (u32)status_ok);

                // Descriptor 0 initialisieren mit req_hdr_pa=0 als Sentinel
                // (nur diagnostisch, wird bei jedem Request ueberschrieben)
                hal::pf("[VIO ] req buffers: hdr=%p data=%p status=%p\n",
                        req_hdr_pa, req_data_pa, req_status_pa);

                ready = true;
                return 0;
            }
        }
    }
    return -1;
}

u64 capacity() { return cap_sectors; }
bool is_ready() { return ready; }
int read_sector(u64 sector, void* buf) { return blk_request(0, sector, 1, buf); }
int write_sector(u64 sector, const void* buf) { return blk_request(1, sector, 1, (void*)buf); }

}  // namespace virtio_blk
