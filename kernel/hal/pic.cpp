#include "kernel/hal/hal.h"

namespace pic {
void remap(){
    hal::outb(0x20, 0x11); hal::outb(0x80, 0);
    hal::outb(0xA0, 0x11); hal::outb(0x80, 0);
    hal::outb(0x21, 0x20); hal::outb(0x80, 0);
    hal::outb(0xA1, 0x28); hal::outb(0x80, 0);
    hal::outb(0x21, 0x04); hal::outb(0x80, 0);
    hal::outb(0xA1, 0x02); hal::outb(0x80, 0);
    hal::outb(0x21, 0x01); hal::outb(0x80, 0);
    hal::outb(0xA1, 0x01); hal::outb(0x80, 0);
    hal::outb(0x21, 0xFF);
    hal::outb(0xA1, 0xFF);
}
void unmask(u8 irq){
    u16 port = (irq < 8) ? 0x21 : 0xA1;
    u8 bit = (irq < 8) ? irq : (irq - 8);
    u8 mask = hal::inb(port) & (u8)~(1u << bit);
    hal::outb(port, mask);
}
void mask(u8 irq){
    u16 port = (irq < 8) ? 0x21 : 0xA1;
    u8 bit = (irq < 8) ? irq : (irq - 8);
    u8 mask = hal::inb(port) | (u8)(1u << bit);
    hal::outb(port, mask);
}
void eoi(u8 irq){
    if (irq >= 8) hal::outb(0xA0, 0x20);
    hal::outb(0x20, 0x20);
}
}
