#pragma once
#include "kernel/hal/hal.h"

namespace virtio_net {
    int  init();
    bool is_ready();
    void get_mac(u8 mac[6]);
    u32  rx_qsize();
    u32  tx_qsize();
}
