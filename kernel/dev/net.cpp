// kernel/dev/net.cpp — A11.3: IPv4 + ICMP + ARP-Cache
#include "kernel/dev/net.h"
#include "kernel/dev/virtio_net.h"
#include "kernel/dev/tcp.h"

extern volatile u64 g_tick;

namespace net {

Iface g_ifaces[4];
u32   g_iface_count = 0;

static u64 stat_rx_ip_pkts = 0;
static u64 stat_tx_ip_pkts = 0;
static u16 ip_id_seq = 0;



static UdpSocket g_udp[8];

static u16 icmp_id  = 0x4E58;   // 'NX'

static void memcpy_n(u8* dst, const u8* src, u32 n) { for (u32 i = 0; i < n; i++) dst[i] = src[i]; }
static void memset_n(u8* dst, u8 v, u32 n) { for (u32 i = 0; i < n; i++) dst[i] = v; }

u16 checksum(const u8* data, u32 len) {
    u32 sum = 0;
    while (len > 1) {
        sum += ((u32)data[0] << 8) | data[1];
        data += 2; len -= 2;
    }
    if (len) sum += (u32)data[0] << 8;
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (u16)(~sum & 0xFFFF);
}

Iface* default_iface() {
    return g_iface_count ? &g_ifaces[0] : nullptr;
}

void arp_cache_put(const u8 ip[4], const u8 mac[6]) {
    Iface* f = default_iface();
    if (!f) return;
    // Erster freier Slot, dann Re-Use
    int slot = -1;
    for (int i = 0; i < 16; i++) {
        if (f->arp[i].valid) {
            if (f->arp[i].ip[0] == ip[0] && f->arp[i].ip[1] == ip[1] &&
                f->arp[i].ip[2] == ip[2] && f->arp[i].ip[3] == ip[3]) {
                slot = i; break;
            }
        } else if (slot < 0) {
            slot = i;
        }
    }
    if (slot < 0) slot = 0;  // overwrite
    memcpy_n(f->arp[slot].ip, ip, 4);
    memcpy_n(f->arp[slot].mac, mac, 6);
    f->arp[slot].age = 0;
    f->arp[slot].valid = true;
}

bool arp_lookup(const u8 ip[4], u8 out_mac[6]) {
    Iface* f = default_iface();
    if (!f) return false;
    for (int i = 0; i < 16; i++) {
        if (!f->arp[i].valid) continue;
        if (f->arp[i].ip[0] == ip[0] && f->arp[i].ip[1] == ip[1] &&
            f->arp[i].ip[2] == ip[2] && f->arp[i].ip[3] == ip[3]) {
            memcpy_n(out_mac, f->arp[i].mac, 6);
            return true;
        }
    }
    return false;
}

int ip_send(const u8 dst[4], u8 proto, const u8* payload, u32 len) {
    Iface* f = default_iface();
    if (!f || !f->up) return -1;
    if (len + 20 + 14 > 1514) return -1;

    u8 mac[6];
    if (!arp_lookup(dst, mac)) {
        int r = virtio_net::arp_resolve(dst, mac);
        if (r != 0) {
            hal::pf("[NET ] ip_send: arp fail for %u.%u.%u.%u (err %d)\n",
                    dst[0], dst[1], dst[2], dst[3], r);
            return -2;
        }
        arp_cache_put(dst, mac);
    }

    u8 frame[1514];
    // Ethernet
    memcpy_n(frame + 0, mac, 6);
    memcpy_n(frame + 6, f->mac, 6);
    frame[12] = 0x08; frame[13] = 0x00;   // IPv4

    // IP-Header
    u8* ip = frame + 14;
    ip[0] = 0x45;
    ip[1] = 0x00;
    u16 tot = (u16)(20 + len);
    ip[2] = tot >> 8; ip[3] = tot & 0xFF;
    u16 id = ++ip_id_seq;
    ip[4] = id >> 8; ip[5] = id & 0xFF;
    ip[6] = 0x40; ip[7] = 0x00;   // DF
    ip[8] = 64;                    // TTL
    ip[9] = proto;
    ip[10] = 0; ip[11] = 0;       // checksum placeholder
    memcpy_n(ip + 12, f->ip, 4);
    memcpy_n(ip + 16, dst, 4);
    u16 csum = checksum(ip, 20);
    ip[10] = csum >> 8; ip[11] = csum & 0xFF;

    // Payload
    if (len) memcpy_n(frame + 34, payload, len);

    int r = virtio_net::net_tx(frame, 14 + 20 + len);
    if (r == 0) stat_tx_ip_pkts++;
    return r;
}

int icmp_ping(const u8 dst[4], u32 timeout_ms) {
    Iface* f = default_iface();
    if (!f) return -1;

    // ICMP echo request: 8 + 32 Datenbytes
    u8 icmp[40];
    icmp[0] = 8;  icmp[1] = 0;
    icmp[2] = 0;  icmp[3] = 0;
    icmp[4] = icmp_id >> 8;
    icmp[5] = icmp_id & 0xFF;
    icmp[6] = 0;  icmp[7] = 1;   // seq=1
    for (int i = 8; i < 40; i++) icmp[i] = (u8)('a' + (i - 8) % 26);
    u16 icmp_csum = checksum(icmp, 40);
    icmp[2] = icmp_csum >> 8;
    icmp[3] = icmp_csum & 0xFF;

    int sr = ip_send(dst, 1, icmp, 40);
    if (sr != 0) {
        hal::pf("[NET ] icmp_ping: send fail (err %d)\n", sr);
        return -2;
    }
    hal::pf("[NET ] ICMP echo request -> %u.%u.%u.%u\n",
            dst[0], dst[1], dst[2], dst[3]);

    // Poll auf Reply (grobes ms-Modell: 1 spin ~ 1us)
    u8 rxbuf[1500];
    u32 max_spins = timeout_ms * 1000;
    for (u32 spin = 0; spin < max_spins; spin += 100) {
        for (int inner = 0; inner < 100; inner++) {
            int n = virtio_net::net_poll(rxbuf, sizeof(rxbuf));
            if (n > 14 + 20) {
                // Ethernet IPv4?
                if (rxbuf[12] == 0x08 && rxbuf[13] == 0x00) {
                    u8* ip = rxbuf + 14;
                    u32 ihl = (ip[0] & 0x0F) * 4;
                    if (ip[9] == 1 && n >= (int)(14 + ihl + 8)) {
                        u8* icmp_rx = ip + ihl;
                        if (icmp_rx[0] == 0 && icmp_rx[1] == 0 &&           // echo reply
                            icmp_rx[4] == (icmp_id >> 8) &&
                            icmp_rx[5] == (icmp_id & 0xFF)) {
                            stat_rx_ip_pkts++;
                            hal::pf("[NET ] ICMP echo reply from %u.%u.%u.%u (ttl=%u, %d bytes)\n",
                                    ip[12], ip[13], ip[14], ip[15],
                                    (u32)ip[8], n - 14);
                            return 0;
                        }
                    }
                }
                // Nicht-ICMP-Paket: verwerfen
                stat_rx_ip_pkts++;
            }
            for (int j = 0; j < 10; j++) __asm__ volatile("" ::: "memory");
        }
    }
    hal::pf("[NET ] ICMP timeout after %u ms\n", timeout_ms);
    return -3;
}

u64 stat_rx_ip() { return stat_rx_ip_pkts; }
u64 stat_tx_ip() { return stat_tx_ip_pkts; }

void init() {
    g_iface_count = 0;
    if (!virtio_net::is_ready()) {
        hal::ps("[NET ] net::init: no NIC, skip\n");
        return;
    }
    Iface* f = &g_ifaces[0];
    // Name
    const char* nm = "eth0";
    for (int i = 0; i < 7; i++) f->name[i] = nm[i] ? nm[i] : 0;

    virtio_net::get_mac(f->mac);

    // QEMU user-net Standard-Config
    u8 ip[4]   = {10, 0, 2, 15};
    u8 mask[4] = {255, 255, 255, 0};
    u8 gw[4]   = {10, 0, 2, 2};
    memcpy_n(f->ip, ip, 4);
    memcpy_n(f->mask, mask, 4);
    memcpy_n(f->gw, gw, 4);
    f->up = true;

    for (int i = 0; i < 16; i++) f->arp[i].valid = false;

    g_iface_count = 1;
    hal::pf("[NET ] iface %s: ip=%u.%u.%u.%u/%u.%u.%u.%u gw=%u.%u.%u.%u\n",
            f->name,
            f->ip[0], f->ip[1], f->ip[2], f->ip[3],
            f->mask[0], f->mask[1], f->mask[2], f->mask[3],
            f->gw[0], f->gw[1], f->gw[2], f->gw[3]);
}


// ── UDP-Socket-Layer ────────────────────────────────────────────

int udp_open(u16 local_port) {
    for (u32 i = 0; i < 8; i++) {
        if (!g_udp[i].used) {
            g_udp[i].used = true;
            g_udp[i].local_port = local_port;
            g_udp[i].rx_head = 0;
            g_udp[i].rx_tail = 0;
            hal::pf("[NET ] udp_open idx=%u port=%u\n", i, (u32)local_port);
            return (int)i;
        }
    }
    return -1;
}

int udp_close(u32 idx) {
    if (idx >= 8) return -1;
    g_udp[idx].used = false;
    return 0;
}

int udp_send(u16 src_port, const u8 dst_ip[4], u16 dst_port,
             const u8* data, u32 len) {
    if (len > 1472) return -1;
    u8 buf[1500];
    buf[0] = (u8)(src_port >> 8); buf[1] = (u8)(src_port & 0xFF);
    buf[2] = (u8)(dst_port >> 8); buf[3] = (u8)(dst_port & 0xFF);
    u16 ulen = (u16)(8 + len);
    buf[4] = (u8)(ulen >> 8); buf[5] = (u8)(ulen & 0xFF);
    buf[6] = 0; buf[7] = 0;   // checksum 0 = legal für IPv4
    for (u32 i = 0; i < len; i++) buf[8 + i] = data[i];
    int r = ip_send(dst_ip, 17, buf, ulen);
    if (r == 0) {
        hal::pf("[NET ] UDP TX -> %u.%u.%u.%u:%u (%u bytes, src=%u)\n",
                dst_ip[0], dst_ip[1], dst_ip[2], dst_ip[3],
                (u32)dst_port, len, (u32)src_port);
    } else {
        hal::pf("[NET ] UDP TX fail (err %d)\n", r);
    }
    return r;
}

void ip_drain() {
    u8 frame[1500];
    for (int iter = 0; iter < 8; iter++) {
        int n = virtio_net::net_poll(frame, sizeof(frame));
        if (n <= 0) break;
        if (n < 14 + 20) continue;
        if (frame[12] != 0x08 || frame[13] != 0x00) continue;
        u8* ip = frame + 14;
        u32 ihl = (u32)(ip[0] & 0x0F) * 4;
        u8 proto = ip[9];
        if (n < (int)(14 + ihl)) continue;
        const u8* payload = ip + ihl;
        u32 paylen = n - 14 - ihl;
        u8 src_ip[4];
        memcpy_n(src_ip, ip + 12, 4);

        if (proto == 17 && paylen >= 8) {
            u16 src_port = ((u16)payload[0] << 8) | payload[1];
            u16 dst_port = ((u16)payload[2] << 8) | payload[3];
            u32 dlen = paylen - 8;
            if (dlen > 256) dlen = 256;
            for (u32 i = 0; i < 8; i++) {
                if (!g_udp[i].used) continue;
                if (g_udp[i].local_port != dst_port) continue;
                u32 slot = g_udp[i].rx_head % 4;
                u32 nxt  = (g_udp[i].rx_head + 1) % 4;
                if (nxt == g_udp[i].rx_tail) break;
                for (u32 k = 0; k < dlen; k++) g_udp[i].rx_buf[slot][k] = payload[8 + k];
                g_udp[i].rx_len[slot] = (u16)dlen;
                memcpy_n(g_udp[i].rx_src_ip[slot], src_ip, 4);
                g_udp[i].rx_src_port[slot] = src_port;
                g_udp[i].rx_head = nxt;
                break;
            }
        } else if (proto == 6) {
            tcp_handle_frame(src_ip, payload, paylen);
        }
    }
}

void udp_drain() { ip_drain(); }

int udp_recv(u32 idx, u8* out_buf, u32 cap,
             u8* out_src_ip, u16* out_src_port, u32 timeout_ms) {
    if (idx >= 8 || !g_udp[idx].used) return -1;
    if (timeout_ms > 60000) timeout_ms = 60000;
    u64 deadline = g_tick + (timeout_ms / 10) + 1;
    for (;;) {
        udp_drain();
        UdpSocket* s = &g_udp[idx];
        if (s->rx_head != s->rx_tail) {
            u32 slot = s->rx_tail % 4;
            u32 n = s->rx_len[slot];
            if (n > cap) n = cap;
            for (u32 k = 0; k < n; k++) out_buf[k] = s->rx_buf[slot][k];
            if (out_src_ip) memcpy_n(out_src_ip, s->rx_src_ip[slot], 4);
            if (out_src_port) *out_src_port = s->rx_src_port[slot];
            s->rx_tail = (s->rx_tail + 1) % 4;
            return (int)n;
        }
        if (g_tick >= deadline) {
            hal::pf("[NET ] udp_recv timeout after %u ms\n", timeout_ms);
            return -2;
        }
        __asm__ volatile("sti; hlt");
    }
}

}  // namespace net
