#pragma once
#include "kernel/hal/hal.h"

namespace virtio_net {
    int  init();
    bool is_ready();
    void get_mac(u8 mac[6]);
    u32  rx_qsize();
    u32  tx_qsize();

    int  net_tx(const u8* frame, u32 len);
    int  net_poll(u8* out, u32 cap);
    int  arp_resolve(const u8 ip[4], u8 out_mac[6]);
    void stats(u64* rx_pkts, u64* tx_pkts, u64* rx_bytes, u64* tx_bytes);
}
