// kernel/dev/http.cpp — A-final-1: HTTP mit Chunked-Encoding
#include "kernel/dev/http.h"
#include "kernel/dev/tcp.h"

namespace net {

static void http_memcpy(u8* d, const u8* s, u32 n) {
    for (u32 i = 0; i < n; i++) d[i] = s[i];
}

// Sucht "\\r\\n\\r\\n" und liefert Offset NACH der Leerzeile (Body-Start).
static int find_header_end(const u8* raw, u32 n) {
    for (u32 i = 0; i + 3 < n; i++) {
        if (raw[i]   == '\r' && raw[i+1] == '\n' &&
            raw[i+2] == '\r' && raw[i+3] == '\n')
            return (int)(i + 4);
    }
    return -1;
}

// Case-insensitive Substring-Suche im Header-Bereich.
static bool header_has(const u8* h, u32 n, const char* needle) {
    u32 nl = 0; while (needle[nl]) nl++;
    if (nl == 0 || nl > n) return false;
    for (u32 i = 0; i + nl <= n; i++) {
        bool m = true;
        for (u32 j = 0; j < nl; j++) {
            u8 a = h[i+j], b = (u8)needle[j];
            if (a >= 'A' && a <= 'Z') a = (u8)(a + 32);
            if (b >= 'A' && b <= 'Z') b = (u8)(b + 32);
            if (a != b) { m = false; break; }
        }
        if (m) return true;
    }
    return false;
}

// Status aus "HTTP/1.x NNN ..." in den ersten ~12 Bytes.
static u32 parse_status(const u8* raw, u32 n) {
    if (n < 12) return 0;
    // Suche nach "HTTP/"
    u32 p = 0;
    for (; p + 4 < n; p++) {
        if (raw[p]=='H' && raw[p+1]=='T' && raw[p+2]=='T' && raw[p+3]=='P')
            break;
    }
    if (p + 4 >= n) return 0;
    // nach "HTTP/x.y " kommt der Code
    u32 sp = p + 4;
    while (sp < n && raw[sp] != ' ') sp++;
    sp++;
    if (sp + 3 > n) return 0;
    if (raw[sp] < '0' || raw[sp] > '9') return 0;
    return (u32)(raw[sp]-'0')*100 + (u32)(raw[sp+1]-'0')*10 + (u32)(raw[sp+2]-'0');
}

// Chunked-Decoder. Format: <hex-len>\\r\\n<data>\\r\\n... 0\\r\\n\\r\\n
static u32 decode_chunked(const u8* in, u32 in_len, u8* out, u32 out_cap) {
    u32 ip = 0, op = 0;
    while (ip < in_len) {
        u32 hex = 0, digits = 0;
        while (ip < in_len) {
            u8 c = in[ip++];
            if (c == '\r') { if (ip < in_len && in[ip] == '\n') ip++; break; }
            if      (c >= '0' && c <= '9') { hex = hex*16 + (u32)(c-'0');      digits++; }
            else if (c >= 'a' && c <= 'f') { hex = hex*16 + (u32)(c-'a') + 10; digits++; }
            else if (c >= 'A' && c <= 'F') { hex = hex*16 + (u32)(c-'A') + 10; digits++; }
            else return op;   // malformed
        }
        if (digits == 0) return op;
        if (hex == 0) return op;   // letzter Chunk
        if (ip + hex > in_len) return op;   // truncated
        for (u32 i = 0; i < hex && op < out_cap; i++) out[op++] = in[ip+i];
        ip += hex;
        if (ip < in_len && in[ip] == '\r') ip++;
        if (ip < in_len && in[ip] == '\n') ip++;
    }
    return op;
}

int http_get(const u8 ip[4], const char* host, u16 port, const char* path,
             u8* body_out, u32 body_cap, u32* body_len_out,
             bool print_headers, u32 timeout_ms) {
    if (body_len_out) *body_len_out = 0;

    int idx = tcp_open();
    if (idx < 0) return -1;

    if (tcp_connect((u32)idx, ip, port) != 0) {
        tcp_close((u32)idx);
        return -2;
    }

    // Request bauen. Host-Header MUSS der Hostname sein, nicht die IP —
    // Cloudflare und die meisten vhosts rejecten "Host: <ip>".
    char req[512];
    u32 rp = 0;
    auto put_str = [&](const char* s) {
        while (*s && rp < sizeof(req)-1) req[rp++] = *s++;
    };
    put_str("GET ");
    put_str(path);
    put_str(" HTTP/1.1\r\nHost: ");
    put_str(host);
    put_str("\r\nConnection: close\r\n\r\n");

    int sr = tcp_send((u32)idx, (const u8*)req, rp);
    if (sr != (int)rp) {
        tcp_close((u32)idx);
        return -3;
    }

    // Raw-Response komplett lesen (16 KB BSS-Puffer)
    static u8 raw[16 * 1024];
    u32 raw_len = 0;
    for (;;) {
        int n = tcp_recv((u32)idx, raw + raw_len,
                         sizeof(raw) - raw_len, timeout_ms);
        if (n < 0) break;   // timeout
        if (n == 0) break;  // EOF
        raw_len += (u32)n;
        if (raw_len >= sizeof(raw)) break;
    }
    tcp_close((u32)idx);

    if (raw_len == 0) return -4;

    u32 status = parse_status(raw, raw_len);
    int hend = find_header_end(raw, raw_len);
    if (hend < 0) {
        if (print_headers) hal::ps("[HTTP] no header end\n");
        return -5;
    }
    u32 hdr_end = (u32)hend;
    u32 hdr_len = hdr_end;

    if (print_headers) {
        for (u32 i = 0; i < hdr_len; i++) hal::pc((char)raw[i]);
    }

    // Chunked?
    if (header_has(raw, hdr_len, "transfer-encoding: chunked")) {
        u32 n = decode_chunked(raw + hdr_end, raw_len - hdr_end,
                               body_out, body_cap);
        if (body_len_out) *body_len_out = n;
        return (int)status;
    }

    // Sonst: Content-Length oder bis EOF
    u32 body_raw = raw_len - hdr_end;
    u32 cp = body_raw > body_cap ? body_cap : body_raw;
    http_memcpy(body_out, raw + hdr_end, cp);
    if (body_len_out) *body_len_out = cp;
    return (int)status;
}

}  // namespace net
