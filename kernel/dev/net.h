#pragma once
#include "kernel/hal/hal.h"

namespace net {

struct ArpEntry {
    u8  ip[4];
    u8  mac[6];
    u32 age;
    bool valid;
};

struct Iface {
    char name[8];
    u8   mac[6];
    u8   ip[4];
    u8   mask[4];
    u8   gw[4];
    bool up;
    ArpEntry arp[16];
};

extern Iface g_ifaces[4];
extern u32   g_iface_count;

struct Ip4Hdr {
    u8  ver_ihl;
    u8  tos;
    u16 total_len;
    u16 id;
    u16 flags_frag;
    u8  ttl;
    u8  proto;
    u16 checksum;
    u8  src[4];
    u8  dst[4];
} __attribute__((packed));

struct IcmpHdr {
    u8  type;
    u8  code;
    u16 checksum;
    u16 id;
    u16 seq;
} __attribute__((packed));

u16  checksum(const u8* data, u32 len);
void init();
Iface* default_iface();
bool arp_lookup(const u8 ip[4], u8 out_mac[6]);
void arp_cache_put(const u8 ip[4], const u8 mac[6]);
int  ip_send(const u8 dst[4], u8 proto, const u8* payload, u32 len);
int  icmp_ping(const u8 dst[4], u32 timeout_ms);
void ip_drain();
u64  stat_rx_ip();

// ── UDP-Socket-Layer (A11.4) ──
struct UdpSocket {
    bool used;
    u16  local_port;
    u8   rx_buf[4][256];
    u16  rx_len[4];
    u8   rx_src_ip[4][4];
    u16  rx_src_port[4];
    u32  rx_head, rx_tail;
};

int  udp_open(u16 local_port);
int  udp_send(u16 src_port, const u8 dst_ip[4], u16 dst_port,
              const u8* data, u32 len);
int  udp_recv(u32 idx, u8* out_buf, u32 cap,
              u8* out_src_ip, u16* out_src_port, u32 timeout_ms);
int  udp_close(u32 idx);
void udp_drain();

u64  stat_tx_ip();

}  // namespace net
