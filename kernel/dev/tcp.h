#pragma once
#include "kernel/hal/hal.h"

namespace net {

struct TcpHdr {
    u16 src_port;
    u16 dst_port;
    u32 seq;
    u32 ack;
    u8  data_off;
    u8  flags;
    u16 window;
    u16 checksum;
    u16 urg;
} __attribute__((packed));

static constexpr u8 TCP_FIN = 0x01;
static constexpr u8 TCP_SYN = 0x02;
static constexpr u8 TCP_RST = 0x04;
static constexpr u8 TCP_PSH = 0x08;
static constexpr u8 TCP_ACK = 0x10;

static constexpr u8 TCP_CLOSED      = 0;
static constexpr u8 TCP_SYN_SENT    = 1;
static constexpr u8 TCP_ESTABLISHED = 2;
static constexpr u8 TCP_FIN_WAIT    = 3;

int  tcp_open();
int  tcp_connect(u32 idx, const u8 remote_ip[4], u16 remote_port);
int  tcp_send(u32 idx, const u8* data, u32 len);
int  tcp_recv(u32 idx, u8* buf, u32 cap, u32 timeout_ms);
int  tcp_close(u32 idx);
u8   tcp_state(u32 idx);
void tcp_handle_frame(const u8 src_ip[4], const u8* tcp, u32 len);

}  // namespace net
