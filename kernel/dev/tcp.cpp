// kernel/dev/tcp.cpp — A11.5: TCP-Client (MVP, kein Retransmit)
#include "kernel/dev/tcp.h"
#include "kernel/dev/net.h"

extern volatile u64 g_tick;

namespace net {

struct TcpSocket {
    bool used;
    u8   state;
    u8   remote_ip[4];
    u16  remote_port;
    u16  local_port;
    u32  snd_nxt;
    u32  rcv_nxt;
    u32  snd_isn;
    u8   rx_buf[8][1460];
    u16  rx_len[4];
    u32  rx_head, rx_tail;
};

static TcpSocket g_tcp[4];
static u32 g_isn_counter = 2000;
static u16 g_ephemeral   = 40000;

static void memcpy_n(u8* d, const u8* s, u32 n) { for (u32 i = 0; i < n; i++) d[i] = s[i]; }

static u16 tcp_checksum(const u8 src_ip[4], const u8 dst_ip[4],
                        const u8* tcp, u32 tcp_len) {
    u32 sum = 0;
    for (int i = 0; i < 4; i += 2) {
        sum += ((u32)src_ip[i] << 8) | src_ip[i + 1];
        sum += ((u32)dst_ip[i] << 8) | dst_ip[i + 1];
    }
    sum += 0x0006;
    sum += (u16)tcp_len;
    u32 i = 0;
    while (i + 1 < tcp_len) {
        sum += ((u32)tcp[i] << 8) | tcp[i + 1];
        i += 2;
    }
    if (i < tcp_len) sum += (u32)tcp[i] << 8;
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (u16)(~sum & 0xFFFF);
}

static int tcp_xmit(TcpSocket* s, u8 flags, const u8* payload, u32 plen) {
    u8 buf[1460 + 20];
    if (plen > 1460) return -1;

    TcpHdr* h = (TcpHdr*)buf;
    h->src_port = ((u16)s->local_port  >> 8) | ((u16)s->local_port  << 8);
    h->dst_port = ((u16)s->remote_port >> 8) | ((u16)s->remote_port << 8);
    h->seq      = ((s->snd_nxt >> 24) & 0xFF) | (((s->snd_nxt >> 16) & 0xFF) << 8)
                | (((s->snd_nxt >>  8) & 0xFF) << 16) | ((s->snd_nxt & 0xFF) << 24);
    h->ack      = ((s->rcv_nxt >> 24) & 0xFF) | (((s->rcv_nxt >> 16) & 0xFF) << 8)
                | (((s->rcv_nxt >>  8) & 0xFF) << 16) | ((s->rcv_nxt & 0xFF) << 24);
    h->data_off = (u8)((20 / 4) << 4);
    h->flags    = flags;
    h->window   = 0x0000 | (1460 >> 8) | ((1460 & 0xFF) << 8);
    h->checksum = 0;
    h->urg      = 0;
    if (plen) memcpy_n(buf + 20, payload, plen);

    u32 tcp_len = 20 + plen;
    Iface* f = default_iface();
    if (!f) return -1;
    u16 ck = tcp_checksum(f->ip, s->remote_ip, buf, tcp_len);
    h->checksum = (u16)((ck >> 8) | (ck << 8));

    int r = ip_send(s->remote_ip, 6, buf, tcp_len);
    if (r == 0) s->snd_nxt += plen;
    if (flags & TCP_SYN) s->snd_nxt += 1;
    if (flags & TCP_FIN) s->snd_nxt += 1;
    return r;
}

int tcp_open() {
    for (u32 i = 0; i < 4; i++) {
        if (!g_tcp[i].used) {
            g_tcp[i].used = true;
            g_tcp[i].state = TCP_CLOSED;
            g_tcp[i].rx_head = g_tcp[i].rx_tail = 0;
            g_tcp[i].local_port = g_ephemeral++;
            hal::pf("[TCP ] open idx=%u lport=%u\n", i, (u32)g_tcp[i].local_port);
            return (int)i;
        }
    }
    return -1;
}

u8 tcp_state(u32 idx) { if (idx >= 4) return 0xFF; return g_tcp[idx].state; }

int tcp_connect(u32 idx, const u8 remote_ip[4], u16 remote_port) {
    if (idx >= 4 || !g_tcp[idx].used) return -1;
    TcpSocket* s = &g_tcp[idx];
    memcpy_n(s->remote_ip, remote_ip, 4);
    s->remote_port = remote_port;
    s->snd_isn = g_isn_counter++;
    s->snd_nxt = s->snd_isn;
    s->rcv_nxt = 0;
    s->state   = TCP_SYN_SENT;
    hal::pf("[TCP ] SYN -> %u.%u.%u.%u:%u (seq=%u)\n",
            remote_ip[0], remote_ip[1], remote_ip[2], remote_ip[3],
            (u32)remote_port, s->snd_isn);
    if (tcp_xmit(s, TCP_SYN, nullptr, 0) != 0) return -2;

    u64 deadline = g_tick + 300;
    for (;;) {
        ip_drain();
        if (s->state == TCP_ESTABLISHED) {
            hal::pf("[TCP ] connected (snd=%u rcv=%u)\n", s->snd_nxt, s->rcv_nxt);
            return 0;
        }
        if (s->state == TCP_CLOSED) return -3;
        if (g_tick >= deadline) { hal::ps("[TCP ] connect timeout\n"); return -4; }
        __asm__ volatile("sti; hlt");
    }
}

int tcp_send(u32 idx, const u8* data, u32 len) {
    if (idx >= 4 || !g_tcp[idx].used) return -1;
    TcpSocket* s = &g_tcp[idx];
    if (s->state != TCP_ESTABLISHED) return -1;
    u32 sent = 0;
    while (sent < len) {
        u32 chunk = len - sent;
        if (chunk > 1024) chunk = 1024;
        if (tcp_xmit(s, TCP_PSH | TCP_ACK, data + sent, chunk) != 0) return -2;
        sent += chunk;
    }
    hal::pf("[TCP ] sent %u bytes\n", len);
    return (int)len;
}

int tcp_recv(u32 idx, u8* buf, u32 cap, u32 timeout_ms) {
    if (idx >= 4 || !g_tcp[idx].used) return -1;
    u64 deadline = g_tick + (timeout_ms / 10) + 1;
    for (;;) {
        ip_drain();
        TcpSocket* s = &g_tcp[idx];
        if (s->rx_head != s->rx_tail) {
            u32 slot = s->rx_tail % 8;
            u32 n = s->rx_len[slot];
            if (n > cap) n = cap;
            for (u32 k = 0; k < n; k++) buf[k] = s->rx_buf[slot][k];
            s->rx_tail = (s->rx_tail + 1) % 8;
            return (int)n;
        }
        if (s->state == TCP_CLOSED) return 0;
        if (g_tick >= deadline) return -2;
        __asm__ volatile("sti; hlt");
    }
}

int tcp_close(u32 idx) {
    if (idx >= 4 || !g_tcp[idx].used) return -1;
    TcpSocket* s = &g_tcp[idx];
    if (s->state == TCP_ESTABLISHED) {
        s->state = TCP_FIN_WAIT;
        tcp_xmit(s, TCP_FIN | TCP_ACK, nullptr, 0);
        hal::ps("[TCP ] FIN ->\n");
    }
    s->used = false;
    s->state = TCP_CLOSED;
    return 0;
}

void tcp_handle_frame(const u8 src_ip[4], const u8* tcp, u32 len) {
    if (len < 20) return;
    const TcpHdr* h = (const TcpHdr*)tcp;
    u16 src_port = (u16)((h->src_port >> 8) | (h->src_port << 8));
    u16 dst_port = (u16)((h->dst_port >> 8) | (h->dst_port << 8));
    u32 seq      = ((u32)h->seq >> 24) | (((u32)h->seq >> 8) & 0xFF00)
                 | (((u32)h->seq << 8) & 0xFF0000) | ((u32)h->seq << 24);
    u8  flags    = h->flags;
    u8  ihl      = (u8)((h->data_off >> 4) * 4);
    if (ihl < 20 || len < ihl) return;
    u32 paylen   = len - ihl;
    const u8* payload = tcp + ihl;

    for (u32 i = 0; i < 8; i++) {
        TcpSocket* s = &g_tcp[i];
        if (!s->used) continue;
        if (s->local_port != dst_port) continue;
        if (s->remote_port != src_port) continue;

        if (flags & TCP_RST) {
            hal::ps("[TCP ] RST received\n");
            s->state = TCP_CLOSED;
            return;
        }
        if ((flags & (TCP_SYN | TCP_ACK)) == (TCP_SYN | TCP_ACK) &&
            s->state == TCP_SYN_SENT) {
            s->rcv_nxt = seq + 1;
            s->state   = TCP_ESTABLISHED;
            tcp_xmit(s, TCP_ACK, nullptr, 0);
            hal::ps("[TCP ] SYN/ACK received\n");
            return;
        }
        if ((flags & TCP_FIN) && s->state >= TCP_ESTABLISHED) {
            // A-final: payload vor state=CLOSED puffern.
            if (paylen > 0) {
                u32 slot = s->rx_head % 8;
                u32 nxt  = (s->rx_head + 1) % 8;
                if (nxt != s->rx_tail) {
                    u32 cp = paylen > 1460 ? 1460 : paylen;
                    for (u32 k = 0; k < cp; k++) s->rx_buf[slot][k] = payload[k];
                    s->rx_len[slot] = (u16)cp;
                    s->rx_head = nxt;
                }
            }
            s->rcv_nxt = seq + paylen + 1;
            tcp_xmit(s, TCP_ACK, nullptr, 0);
            s->state = TCP_CLOSED;
            hal::ps("[TCP ] FIN received, closed\n");
            return;
        }
        if (s->state == TCP_ESTABLISHED && paylen > 0) {
            u32 slot = s->rx_head % 8;
            u32 nxt  = (s->rx_head + 1) % 8;
            if (nxt != s->rx_tail) {
                u32 cp = paylen > 1460 ? 1460 : paylen;
                for (u32 k = 0; k < cp; k++) s->rx_buf[slot][k] = payload[k];
                s->rx_len[slot] = (u16)cp;
                s->rx_head = nxt;
            }
                s->rcv_nxt = seq + paylen;      // DATA-Branch: keine +1 (FIN kommt im FIN-Branch)
            tcp_xmit(s, TCP_ACK, nullptr, 0);
            return;
        }
        if (flags & TCP_ACK) return;
    }
}

}  // namespace net
