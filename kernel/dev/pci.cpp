// kernel/dev/pci.cpp
#include "kernel/dev/pci.h"

namespace pci {








const char* class_name(u8 cls) {
    switch (cls) {
        case 0x00: return "unclassified";
        case 0x01: return "storage";
        case 0x02: return "network";
        case 0x03: return "display";
        case 0x04: return "multimedia";
        case 0x05: return "memory";
        case 0x06: return "bridge";
        case 0x07: return "comm";
        case 0x08: return "system";
        case 0x09: return "input";
        case 0x0B: return "processor";
        case 0x0C: return "serial";
        case 0x0D: return "wireless";
        case 0x10: return "crypto";
        case 0x40: return "coprocessor";
        default:   return "?";
    }
}

static u32 bar_size(u8 bus, u8 dev, u8 func, int idx) {
    u8 off = (u8)(0x10 + idx * 4);
    u32 orig = cfg_read32(bus, dev, func, off);
    if (orig == 0) return 0;
    cfg_write32(bus, dev, func, off, 0xFFFFFFFF);
    u32 mask = cfg_read32(bus, dev, func, off);
    cfg_write32(bus, dev, func, off, orig);
    if (mask == 0 || mask == 0xFFFFFFFF) return 0;
    if (orig & 1) return ~(mask & ~0x3u) + 1;
    return ~(mask & ~0xFu) + 1;
}

static int read_device(u8 bus, u8 dev, u8 func, Device* d) {
    u16 vendor = cfg_read16(bus, dev, func, 0x00);
    if (vendor == 0xFFFF) return 0;
    d->bus  = bus;
    d->dev  = dev;
    d->func = func;
    d->vendor = vendor;
    d->device = cfg_read16(bus, dev, func, 0x02);
    u32 cls = cfg_read32(bus, dev, func, 0x08);
    d->revision   = (u8)(cls & 0xFF);
    d->prog_if    = (u8)((cls >>  8) & 0xFF);
    d->subclass   = (u8)((cls >> 16) & 0xFF);
    d->class_code = (u8)((cls >> 24) & 0xFF);
    d->header_type = (u8)((cfg_read32(bus, dev, func, 0x0C) >> 16) & 0xFF);
    for (int i = 0; i < 6; i++) {
        u32 raw = cfg_read32(bus, dev, func, (u8)(0x10 + i * 4));
        d->bar[i] = raw & ~0xFu;
        d->bar_size[i] = bar_size(bus, dev, func, i);
    }
    return 1;
}

static u32 g_count = 0;
using Visitor = void(*)(const Device*, void*);

void scan(Visitor v, void* ctx) {
    g_count = 0;
    for (u32 bus = 0; bus < 256; bus++) {
        for (u32 dev = 0; dev < 32; dev++) {
            Device d0{};
            if (!read_device((u8)bus, (u8)dev, 0, &d0)) continue;
            v(&d0, ctx); g_count++;
            if (d0.header_type & 0x80) {
                for (u32 func = 1; func < 8; func++) {
                    Device d{};
                    if (!read_device((u8)bus, (u8)dev, (u8)func, &d)) continue;
                    v(&d, ctx); g_count++;
                }
            }
        }
    }
}

u32 count() { return g_count; }

void init() {
    u32 c = 0;
    for (u32 dev = 0; dev < 32; dev++)
        if (cfg_read16(0, (u8)dev, 0, 0x00) != 0xFFFF) c++;
    hal::pf("[PCI] bus 0: %u devices present\n", c);
}

} // namespace pci
