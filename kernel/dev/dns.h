#pragma once
#include "kernel/hal/hal.h"

namespace net {

// Loest hostname zu IPv4 auf via DNS-Server (typisch 10.0.2.3 in QEMU).
// Rueckgabe: 0 = OK (out_ip gefuellt), <0 = Fehler.
int  dns_resolve(const char* hostname, u8 out_ip[4]);
int  dns_resolve_via(const char* hostname, const u8 dns_ip[4],
                     u8 out_ip[4]);

}  // namespace net
