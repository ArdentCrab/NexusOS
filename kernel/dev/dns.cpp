// kernel/dev/dns.cpp — A11.6: DNS-Resolver (minimal, A-Record only)
#include "kernel/dev/dns.h"
#include "kernel/dev/net.h"

namespace net {

extern volatile u64 g_tick;

static u16 g_dns_id_counter = 0x4E58;   // 'NX'

// Baut Query: Header(12) + QNAME(labels) + QTYPE(2) + QCLASS(2)
static u32 build_query(u8* buf, const char* hostname, u16 id) {
    u32 p = 0;
    // Header
    buf[p++] = (u8)(id >> 8);
    buf[p++] = (u8)(id & 0xFF);
    buf[p++] = 0x01;  buf[p++] = 0x00;    // flags: RD=1
    buf[p++] = 0x00;  buf[p++] = 0x01;    // QDCOUNT = 1
    buf[p++] = 0x00;  buf[p++] = 0x00;    // ANCOUNT
    buf[p++] = 0x00;  buf[p++] = 0x00;    // NSCOUNT
    buf[p++] = 0x00;  buf[p++] = 0x00;    // ARCOUNT

    // QNAME: "example.com" -> 7 "example" 3 "com" 0
    u32 label_start = p++;
    u32 label_len = 0;
    for (const char* h = hostname; ; h++) {
        if (*h == '.' || *h == 0) {
            if (label_len > 63) return 0;
            buf[label_start] = (u8)label_len;
            label_len = 0;
            if (*h == 0) break;
            label_start = p++;
        } else {
            buf[p++] = (u8)*h;
            label_len++;
        }
        if (p > 200) return 0;
    }
    buf[p++] = 0x00;   // QNAME-End

    // QTYPE = A (1)
    buf[p++] = 0x00; buf[p++] = 0x01;
    // QCLASS = IN (1)
    buf[p++] = 0x00; buf[p++] = 0x01;
    return p;
}

// Skippt einen Domain-Namen (entweder Labels oder Compression-Pointer).
static bool skip_name(const u8* buf, u32 n, u32& pos) {
    u32 guard = 0;
    while (pos < n && guard++ < 200) {
        u8 b = buf[pos];
        if ((b & 0xC0) == 0xC0) { pos += 2; return pos <= n; }  // pointer
        if (b == 0) { pos++; return true; }
        pos += b + 1;
    }
    return false;
}

// Parst DNS-Antwort. Sucht ersten A-Record (Type=1, Class=1, RdLength=4).
static int parse_response(const u8* buf, u32 n, u16 expected_id, u8 out_ip[4]) {
    if (n < 12) return -10;
    u16 id     = ((u16)buf[0] << 8) | buf[1];
    u16 flags  = ((u16)buf[2] << 8) | buf[3];
    u16 qd     = ((u16)buf[4] << 8) | buf[5];
    u16 an     = ((u16)buf[6] << 8) | buf[7];

    if (id != expected_id) return -11;
    if ((flags & 0x8000) == 0) return -12;   // kein Response-Flag
    if ((flags & 0x000F) != 0) return -13;   // RCODE != 0

    u32 pos = 12;
    // Question(s) skippen
    for (u16 q = 0; q < qd; q++) {
        if (!skip_name(buf, n, pos)) return -14;
        if (pos + 4 > n) return -15;
        pos += 4;   // QTYPE + QCLASS
    }
    // Answer(s) parsen
    for (u16 a = 0; a < an; a++) {
        if (!skip_name(buf, n, pos)) return -16;
        if (pos + 10 > n) return -17;
        u16 type = ((u16)buf[pos] << 8) | buf[pos+1]; pos += 2;
        u16 cls  = ((u16)buf[pos] << 8) | buf[pos+1]; pos += 2;
        // TTL (4)
        pos += 4;
        u16 rdlen = ((u16)buf[pos] << 8) | buf[pos+1]; pos += 2;
        if (pos + rdlen > n) return -18;
        if (type == 1 && cls == 1 && rdlen == 4) {
            out_ip[0] = buf[pos];
            out_ip[1] = buf[pos+1];
            out_ip[2] = buf[pos+2];
            out_ip[3] = buf[pos+3];
            return 0;
        }
        pos += rdlen;
    }
    return -19;   // kein A-Record gefunden
}

int dns_resolve_via(const char* hostname, const u8 dns_ip[4], u8 out_ip[4]) {
    if (!hostname || !hostname[0]) return -1;

    // Freien UDP-Port suchen und Socket oeffnen
    int idx = -1;
    u16 port = 0;
    for (u16 p = 50000; p < 50100; p++) {
        idx = udp_open(p);
        if (idx >= 0) { port = p; break; }
    }
    if (idx < 0) return -2;

    u16 id = ++g_dns_id_counter;
    u8 query[256];
    u32 qlen = build_query(query, hostname, id);
    if (qlen == 0) { udp_close((u32)idx); return -3; }

    hal::pf("[DNS ] query '%s' id=0x%x qlen=%u (sport=%u)\n",
            hostname, (u32)id, qlen, (u32)port);

    if (udp_send(port, dns_ip, 53, query, qlen) != 0) {
        udp_close((u32)idx);
        return -4;
    }

    u8 resp[512];
    u8 src_ip[4];
    u16 src_port = 0;
    int n = udp_recv((u32)idx, resp, sizeof(resp),
                     src_ip, &src_port, 3000);
    udp_close((u32)idx);

    if (n < 0) {
        hal::pf("[DNS ] no response (err %d)\n", n);
        return -5;
    }
    hal::pf("[DNS ] response %d bytes from %u.%u.%u.%u:%u\n",
            n, src_ip[0], src_ip[1], src_ip[2], src_ip[3], (u32)src_port);

    int r = parse_response(resp, (u32)n, id, out_ip);
    if (r == 0) {
        hal::pf("[DNS ] %s -> %u.%u.%u.%u\n",
                hostname, out_ip[0], out_ip[1], out_ip[2], out_ip[3]);
    } else {
        hal::pf("[DNS ] parse fail (err %d)\n", r);
    }
    return r;
}

int dns_resolve(const char* hostname, u8 out_ip[4]) {
    u8 dns_ip[4] = {10, 0, 2, 3};   // QEMU-SLIRP DNS
    return dns_resolve_via(hostname, dns_ip, out_ip);
}

}  // namespace net
