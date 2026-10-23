#pragma once
#include "kernel/hal/hal.h"

namespace net {

// HTTP/1.1 GET. Folge:
//   1. tcp_open + tcp_connect + GET senden
//   2. Response komplett lesen (bis EOF)
//   3. Header/Body trennen, Chunked-Encoding dekodieren wenn vorhanden
//   4. Body in body_out schreiben
//
// print_headers=true: Header-Bytes werden auf serieller Konsole ausgegeben.
// Rueckgabe: HTTP-Status (200, 301, 404, ...) oder <0 für Transport-Fehler.
//
// Bekannte Limitierung: Content-Encoding gzip wird nicht dekodiert (Body
// bleibt komprimiert). Wird in A-S2 nachgezogen.
int http_get(const u8 ip[4], const char* host, u16 port, const char* path,
             u8* body_out, u32 body_cap, u32* body_len_out,
             bool print_headers, u32 timeout_ms);

}  // namespace net
