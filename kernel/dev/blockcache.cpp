// kernel/dev/blockcache.cpp
#include "kernel/dev/blockcache.h"
#include "kernel/dev/virtio_blk.h"

namespace blockcache {

static constexpr int NSLOTS = 64;

struct Entry {
    u64  lba;
    u64  age;
    u8   data[512];
    bool valid;
    bool dirty;
    u16  _pad;
};

static Entry slots[NSLOTS];
static u64 g_age    = 0;
static u64 g_hits   = 0;
static u64 g_misses = 0;
static u64 g_evict  = 0;

static Entry* find(u64 lba) {
    for (int i = 0; i < NSLOTS; i++)
        if (slots[i].valid && slots[i].lba == lba) return &slots[i];
    return nullptr;
}

static Entry* evict() {
    Entry* oldest = &slots[0];
    for (int i = 1; i < NSLOTS; i++) {
        if (!slots[i].valid) return &slots[i];
        if (slots[i].age < oldest->age) oldest = &slots[i];
    }
    // Cache voll: aeltestes raus, ggf. flushen.
    // Fehler beim Flush: Eintrag NICHT verwerfen, nullptr zurueck.
    if (oldest->dirty) {
        int r = virtio_blk::write_sector(oldest->lba, oldest->data);
        if (r != 0) return nullptr;
        oldest->dirty = false;
        g_evict++;
    }
    return oldest;
}

int read(u64 lba, void* buf) {
    Entry* e = find(lba);
    if (e) {
        g_hits++;
        e->age = ++g_age;
    } else {
        g_misses++;
        e = evict();
        if (!e) return -1;   // Cache voll, Flush fehlgeschlagen
        int r = virtio_blk::read_sector(lba, e->data);
        if (r != 0) {
            // Kein gueltiger Eintrag erzeugen
            return -1;
        }
        e->lba   = lba;
        e->age   = ++g_age;
        e->valid = true;
        e->dirty = false;
    }
    u8* dst = (u8*)buf;
    for (int i = 0; i < 512; i++) dst[i] = e->data[i];
    return 0;
}

int write(u64 lba, const void* buf) {
    Entry* e = find(lba);
    if (e) {
        g_hits++;
        e->age = ++g_age;
    } else {
        g_misses++;
        e = evict();
        if (!e) return -1;
        e->lba   = lba;
        e->age   = ++g_age;
        e->valid = true;
        e->dirty = false;
    }
    const u8* src = (const u8*)buf;
    for (int i = 0; i < 512; i++) e->data[i] = src[i];
    e->dirty = true;
    return 0;
}

int flush() {
    int n = 0;
    for (int i = 0; i < NSLOTS; i++) {
        if (slots[i].valid && slots[i].dirty) {
            int r = virtio_blk::write_sector(slots[i].lba, slots[i].data);
            if (r == 0) {
                slots[i].dirty = false;
                n++;
            }
            // bei Fehler: dirty bleibt, naechster flush versucht erneut
        }
    }
    return n;
}

void invalidate_all() {
    for (int i = 0; i < NSLOTS; i++) {
        slots[i].valid = false;
        slots[i].dirty = false;
        slots[i].age   = 0;
    }
}

u64 hits()    { return g_hits; }
u64 misses()  { return g_misses; }
u64 evicted() { return g_evict; }
int dirty_count() {
    int n = 0;
    for (int i = 0; i < NSLOTS; i++)
        if (slots[i].valid && slots[i].dirty) n++;
    return n;
}
int valid_count() {
    int n = 0;
    for (int i = 0; i < NSLOTS; i++)
        if (slots[i].valid) n++;
    return n;
}

}  // namespace blockcache
