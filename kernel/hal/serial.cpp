#include "kernel/hal/hal.h"

namespace hal {

volatile u32 g_serial_depth = 0;
u64          g_serial_flags = 0;
u32          g_log_level    = LOG_INFO;

static constexpr int LOG_LINES    = 64;
static constexpr int LOG_LINE_LEN = 96;
static char log_buf[LOG_LINES][LOG_LINE_LEN];
static int  log_head  = 0;
static int  log_count = 0;
static int  log_pos   = 0;
static char g_log_prefix = 'I';

extern "C" volatile u32 g_shell_input_active = 0;

static constexpr u32 STREAM_BUF_SIZE = 1024;
static char g_stream_buf[STREAM_BUF_SIZE];
static u32  g_stream_head = 0, g_stream_tail = 0;
static bool g_log_in_flush = false;

static inline void pc_raw_force(char c) {
    while(!(inb(0x3F8+5)&0x20)){} outb(0x3F8,(u8)c);
}

static bool buffer_char(char c) {
    if (!g_shell_input_active) return false;
    if (g_log_in_flush) return false;
    u32 nxt = (g_stream_head + 1) % STREAM_BUF_SIZE;
    if (nxt == g_stream_tail) return true;
    g_stream_buf[g_stream_head] = c;
    g_stream_head = nxt;
    return true;
}

static inline void pc_raw(char c){
    if (buffer_char(c)) return;
    pc_raw_force(c);
}
static void ps_raw(const char* s){ while(*s) pc_raw(*s++); }
static void pu_raw_w(u64 v, u32 base, int width, char pad){
    char b[24]; int n=0; const char* d="0123456789abcdef";
    if(!v) b[n++]='0';
    while(v){ b[n++]=d[v%base]; v/=base; }
    while(n < width) b[n++] = pad;
    while(n--) pc_raw(b[n]);
}
static void pu_raw(u64 v, u32 base){ pu_raw_w(v, base, 0, '0'); }

void serial_init(){
    outb(0x3F8+1,0); outb(0x3F8+3,0x80);
    outb(0x3F8,1); outb(0x3F8+1,0);
    outb(0x3F8+3,3); outb(0x3F8+2,0xC7); outb(0x3F8+4,0x0B);
}

void serial_write_bulk_raw(const char* buf, u64 len) {
    for (u64 i = 0; i < len; i++) pc_raw(buf[i]);
}

void pc(char c){ serial_lock(); pc_raw(c); serial_unlock(); }
void ps(const char* s){ serial_lock(); ps_raw(s); serial_unlock(); }

using va_list = __builtin_va_list;

void pf(const char* f, ...){
    serial_lock();
    va_list ap; __builtin_va_start(ap,f);
    for(;*f;f++){
        if(*f!='%'){ pc_raw(*f); continue; }
        f++;
        bool left = false; char pad = ' '; int width = 0;
        if (*f == '-') { left = true; f++; }
        if (*f == '0') { pad = '0'; f++; }
        while (*f >= '0' && *f <= '9') { width = width*10 + (*f-'0'); f++; }
        if(*f=='l'){
            f++;
            if(*f=='d'){ i64 v=__builtin_va_arg(ap,i64);
                if(v<0){pc_raw('-'); v=-v;}
                if(width){ pu_raw_w((u64)v,10,width,pad); } else pu_raw((u64)v,10); }
            else if(*f=='u'){ if(width){ pu_raw_w(__builtin_va_arg(ap,u64),10,width,pad); } else pu_raw(__builtin_va_arg(ap,u64),10); }
            else if(*f=='x'){ if(width){ pu_raw_w(__builtin_va_arg(ap,u64),16,width,pad); } else pu_raw(__builtin_va_arg(ap,u64),16); }
            else { pc_raw('%'); pc_raw('l'); pc_raw(*f); }
        }
        else if(*f=='d'){ i32 v=__builtin_va_arg(ap,i32);
            if(v<0){pc_raw('-'); v=-v;}
            if(width){ pu_raw_w((u64)(u32)v,10,width,pad); } else pu_raw((u64)(u32)v,10); }
        else if(*f=='u'){ if(width){ pu_raw_w(__builtin_va_arg(ap,u32),10,width,pad); } else pu_raw(__builtin_va_arg(ap,u32),10); }
        else if(*f=='x'){ if(width){ pu_raw_w(__builtin_va_arg(ap,u32),16,width,pad); } else pu_raw(__builtin_va_arg(ap,u32),16); }
        else if(*f=='p'){ pc_raw('0'); pc_raw('x'); pu_raw(__builtin_va_arg(ap,u64),16); }
        else if(*f=='s'){
            const char* str = __builtin_va_arg(ap,const char*);
            int len = 0; while (str[len]) len++;
            int padn = (width > len) ? (width - len) : 0;
            if (!left) for (int i = 0; i < padn; i++) pc_raw(' ');
            ps_raw(str);
            if (left) for (int i = 0; i < padn; i++) pc_raw(' ');
        }
        else if(*f=='c'){
            char c = (char)__builtin_va_arg(ap,int);
            int padn = (width > 1) ? (width - 1) : 0;
            if (!left) for (int i = 0; i < padn; i++) pc_raw(' ');
            pc_raw(c);
            if (left) for (int i = 0; i < padn; i++) pc_raw(' ');
        }
        else if(*f=='%') pc_raw('%');
        else { pc_raw('%'); pc_raw(*f); }
    }
    __builtin_va_end(ap);
    serial_unlock();
}

bool stream_pending() { return g_stream_head != g_stream_tail; }

void log_flush_and_redraw(const char* prompt, const char* line, u32 len) {
    if (!g_shell_input_active) return;
    if (g_stream_head == g_stream_tail) return;
    serial_lock();
    g_log_in_flush = true;
    pc_raw_force('\r');
    pc_raw_force(0x1B); pc_raw_force('['); pc_raw_force('K');
    while (g_stream_tail != g_stream_head) {
        char c = g_stream_buf[g_stream_tail];
        g_stream_tail = (g_stream_tail + 1) % STREAM_BUF_SIZE;
        pc_raw_force(c);
    }
    if (prompt) { while (*prompt) pc_raw_force(*prompt++); }
    for (u32 i = 0; i < len; i++) pc_raw_force(line[i]);
    g_log_in_flush = false;
    serial_unlock();
}

void echo_pc(char c) { serial_lock(); pc_raw_force(c); serial_unlock(); }
void echo_ps(const char* s) {
    serial_lock();
    while (*s) pc_raw_force(*s++);
    serial_unlock();
}

// --- Log-Ringpuffer ---
static void log_putc(char c) {
    if (c == '\n') {
        log_buf[log_head][log_pos] = 0;
        log_head = (log_head + 1) % LOG_LINES;
        if (log_count < LOG_LINES) log_count++;
        log_pos = 0;
        log_buf[log_head][0] = 0;
    } else if (c == '\r') {
        log_pos = 0;
    } else if (log_pos < LOG_LINE_LEN - 1) {
        log_buf[log_head][log_pos++] = c;
        log_buf[log_head][log_pos] = 0;
    }
}

void klogf(u32 level, const char* f, ...) {
    serial_lock();
    char pch = '?';
    switch (level) {
        case LOG_DEBUG: pch = 'D'; break;
        case LOG_INFO:  pch = 'I'; break;
        case LOG_WARN:  pch = 'W'; break;
        case LOG_ERROR: pch = 'E'; break;
    }
    g_log_prefix = pch;
    bool emit_serial = (level >= g_log_level);

    log_putc(pch);
    log_putc(' ');
    if (emit_serial) { pc_raw(pch); pc_raw(' '); }

    va_list ap; __builtin_va_start(ap, f);
    for (; *f; f++) {
        if (*f != '%') { log_putc(*f); if (emit_serial) pc_raw(*f); continue; }
        f++;
        if (*f == 'd') {
            i64 v = __builtin_va_arg(ap, i64);
            if (v < 0) { log_putc('-'); if (emit_serial) pc_raw('-'); v = -v; }
            char tb[24]; int n = 0;
            if (v == 0) tb[n++] = '0';
            while (v) { tb[n++] = '0' + (v % 10); v /= 10; }
            while (n--) { log_putc(tb[n]); if (emit_serial) pc_raw(tb[n]); }
        }
        else if (*f == 'u') {
            u64 v = __builtin_va_arg(ap, u64);
            char tb[24]; int n = 0;
            if (v == 0) tb[n++] = '0';
            while (v) { tb[n++] = '0' + (v % 10); v /= 10; }
            while (n--) { log_putc(tb[n]); if (emit_serial) pc_raw(tb[n]); }
        }
        else if (*f == 'x') {
            u64 v = __builtin_va_arg(ap, u64);
            char tb[24]; int n = 0;
            const char* hx = "0123456789abcdef";
            if (v == 0) tb[n++] = '0';
            while (v) { tb[n++] = hx[v & 0xF]; v >>= 4; }
            while (n--) { log_putc(tb[n]); if (emit_serial) pc_raw(tb[n]); }
        }
        else if (*f == 'p') {
            log_putc('0'); log_putc('x');
            if (emit_serial) { pc_raw('0'); pc_raw('x'); }
            u64 v = __builtin_va_arg(ap, u64);
            char tb[24]; int n = 0;
            const char* hx = "0123456789abcdef";
            if (v == 0) tb[n++] = '0';
            while (v) { tb[n++] = hx[v & 0xF]; v >>= 4; }
            while (n--) { log_putc(tb[n]); if (emit_serial) pc_raw(tb[n]); }
        }
        else if (*f == 's') {
            const char* str = __builtin_va_arg(ap, const char*);
            while (*str) { log_putc(*str); if (emit_serial) pc_raw(*str); str++; }
        }
        else if (*f == 'c') {
            char c = (char)__builtin_va_arg(ap, int);
            log_putc(c); if (emit_serial) pc_raw(c);
        }
        else if (*f == '%') {
            log_putc('%'); if (emit_serial) pc_raw('%');
        }
        else {
            log_putc('%'); log_putc(*f);
            if (emit_serial) { pc_raw('%'); pc_raw(*f); }
        }
    }
    __builtin_va_end(ap);
    serial_unlock();
}

void log_dump(int n) {
    serial_lock();
    if (n > log_count) n = log_count;
    if (n <= 0) { serial_unlock(); return; }   // FIX: Lock bei early-return freigeben
    int start = (log_head - n + LOG_LINES) % LOG_LINES;
    for (int i = 0; i < n; i++) {
        const char* line = log_buf[(start + i) % LOG_LINES];
        while (*line) pc_raw(*line++);
        pc_raw('\n');
    }
    serial_unlock();
}

[[noreturn]] void panic(const char* m){
    __asm__ volatile("cli");
    ps("\n*** PANIC: "); ps(m); ps(" ***\n");
    for(;;) __asm__ volatile("hlt");
}

} // namespace hal
