// kernel/dev/pci.h
#ifndef KERNEL_DEV_PCI_H
#define KERNEL_DEV_PCI_H
#include "kernel/hal/hal.h"
namespace pci {
struct Device {
    u8 bus, dev, func; u16 vendor, device;
    u8 class_code, subclass, prog_if, revision, header_type;
    u32 bar[6]; u32 bar_size[6];
};
static inline u32 cfg_read32(u8 bus, u8 dev, u8 func, u8 off) {
    u32 addr = 0x80000000u | ((u32)bus << 16) | ((u32)dev << 11) | ((u32)func << 8) | (off & 0xFC);
    hal::outl(0xCF8, addr); return hal::inl(0xCFC);
}
static inline u16 cfg_read16(u8 bus, u8 dev, u8 func, u8 off) {
    return (u16)(cfg_read32(bus, dev, func, (u8)(off & 0xFC)) >> ((off & 2) * 8));
}
static inline void cfg_write32(u8 bus, u8 dev, u8 func, u8 off, u32 v) {
    u32 addr = 0x80000000u | ((u32)bus << 16) | ((u32)dev << 11) | ((u32)func << 8) | (off & 0xFC);
    hal::outl(0xCF8, addr); hal::outl(0xCFC, v);
}
const char* class_name(u8 cls);
using Visitor = void(*)(const Device*, void*);
void scan(Visitor v, void* ctx);
u32 count();
void init();
}
#endif
