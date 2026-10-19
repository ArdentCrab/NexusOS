// kernel/dev/kbd.cpp
#include "kernel/dev/kbd.h"

namespace kbd {
static constexpr u32 BUF_SIZE = 256;
static volatile u8  buf[BUF_SIZE];
static volatile u32 head = 0, tail = 0;

// US-Layout Scancode Set 1 → ASCII (nur die wichtigsten Tasten)
static const char map_normal[128] = {
      0,  27, '1','2','3','4','5','6','7','8','9','0','-','=', '\b',
    '\t','q','w','e','r','t','y','u','i','o','p','[',']','\n',
      0, 'a','s','d','f','g','h','j','k','l',';','\'','`',
      0, '\\','z','x','c','v','b','n','m',',','.','/',
      0, '*',  0, ' ',
};
static const char map_shift[128] = {
      0,  27, '!','@','#','$','%','^','&','*','(',')','_','+', '\b',
    '\t','Q','W','E','R','T','Y','U','I','O','P','{','}','\n',
      0, 'A','S','D','F','G','H','J','K','L',':','"','~',
      0, '|','Z','X','C','V','B','N','M','<','>','?',
      0, '*',  0, ' ',
};

static bool shift = false;
static bool caps  = false;

void irq_handler(){
    u8 sc = hal::inb(0x60);

    // Shift / Caps / Release-Bit
    if (sc == 0x2A || sc == 0x36) { shift = true; return; }
    if (sc == 0xAA || sc == 0xB6) { shift = false; return; }
    if (sc == 0x3A) { caps = !caps; return; }
    if (sc & 0x80) return;   // andere Release-Events ignorieren

    if (sc >= 128) return;
    char c = shift ? map_shift[sc] : map_normal[sc];
    if (c == 0) return;
    if (caps && c >= 'a' && c <= 'z') c = c - 'a' + 'A';

    u32 nh = (head + 1) % BUF_SIZE;
    if (nh != tail) {
        buf[head] = (u8)c;
        head = nh;
    }
}

// Nichtblockierend: -1 wenn leer. Liest PS/2- und seriellen Input.
int getchar_nb(){
    // Erst PS/2
    if (tail != head) {
        char c = (char)buf[tail];
        tail = (tail + 1) % BUF_SIZE;
        return (int)c;
    }
    // Dann serieller Port (QEMU -serial stdio)
    // LSR bit 0: Data Ready
    if (hal::inb(0x3F8 + 5) & 0x01) {
        char c = (char)hal::inb(0x3F8);
        return (int)c;
    }
    return -1;
}

// Wartet bis zu ~300us auf das naechste Zeichen. Fuer Escape-Sequenzen.
int getchar_nb_wait() {
    for (int i = 0; i < 3000; i++) {
        int c = getchar_nb();
        if (c >= 0) return c;
        for (int j = 0; j < 50; j++) __asm__ volatile("" ::: "memory");
    }
    return -1;
}
} // namespace kbd
