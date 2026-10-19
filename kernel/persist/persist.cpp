// kernel/persist/persist.cpp — Refactor Session 6
#include "kernel/persist/persist.h"
#include "kernel/idx/idx.h"
#include "kernel/fs/fs.h"

// Forward-Decls fuer dev/ (S7); verschwinden mit kernel/dev/.
namespace virtio_blk {
bool is_ready();
u64  capacity();
int  read_sector(u64 sector, void* buf);
int  write_sector(u64 sector, const void* buf);
}
namespace blockcache {
int  read(u64 lba, void* buf);
int  write(u64 lba, const void* buf);
int  flush();
int  dirty_count();
}

extern volatile u64 g_tick;

// ---------------------------------------------------------------------------
// PERSIST — Superblock, Snapshot, Loader
// ---------------------------------------------------------------------------
namespace persist {

struct __attribute__((packed)) Superblock {
    char magic[8];       // "NEXOS___"
    u32  version;
    u32  pad0;
    u64  snap_seq;
    u32  snap_start;
    u32  snap_count;
    u32  wal_start;
    u32  wal_head;
    u32  wal_tail;
    u32  pad1;
    u32  crc;
};

static constexpr u64 SNAP_START   = 4;
static constexpr u32 RECS_PER_SECTOR = 16;

static constexpr u32 WAL_MAX_SECTORS = 1024;
static constexpr u32 WAL_EPS         = 15;
static constexpr u32 WAL_MAGIC       = 0x57414C53;

static u64 g_snap_seq = 0;

static u32 sb_crc(const u8* p, u32 n) {
    u32 h = 0x811c9dc5;
    for (u32 i = 0; i < n; i++) { h ^= p[i]; h *= 0x01000193; }
    return h;
}

struct WriteCtx {
    u8  buf[512];
    u32 buf_fill;
    u32 sector;
    u32 total;
    bool error;
};

static void write_record(WriteCtx* w, u64 key, u32 type, u64 a, u64 b) {
    u8* p = w->buf + w->buf_fill;
    *(u64*)(p + 0)  = key;
    *(u32*)(p + 8)  = type;
    *(u32*)(p + 12) = 0;
    *(u64*)(p + 16) = a;
    *(u64*)(p + 24) = b;
    w->buf_fill += 32;
    w->total++;
    if (w->buf_fill == 512) {
        if (blockcache::write(w->sector++, w->buf) != 0)
            w->error = true;
        w->buf_fill = 0;
    }
}

static void flush_partial(WriteCtx* w) {
    if (w->buf_fill > 0) {
        while (w->buf_fill < 512) w->buf[w->buf_fill++] = 0;
        if (blockcache::write(w->sector++, w->buf) != 0)
            w->error = true;
        w->buf_fill = 0;
    }
}

struct CollectCtx { WriteCtx* w; };
static void collect_visit(idx::Rec* r, void* ctx) {
    // Persistente Typen: CONFIG (1), SYS (4), FILE (5), NAME (6)
    if (r->type != 1 && r->type != 4 && r->type != 5 && r->type != 6) return;
    CollectCtx* c = (CollectCtx*)ctx;
    write_record(c->w, r->key, r->type, r->a, r->b);
}

struct WalEntry {
    u64 key;
    u64 a;
    u64 b;
    u8  op;
    u8  type;
    u16 _pad;
    u32 _pad2;
};

static u32  g_wal_sector     = WAL_START;
static u32  g_wal_count      = 0;
static u32  g_wal_head_saved = 0;
static u8   g_wal_buf[512];
static bool g_in_recovery    = false;

struct RecoveryGuard {
    bool prev;
    RecoveryGuard()  : prev(g_in_recovery) { g_in_recovery = true; }
    ~RecoveryGuard() { g_in_recovery = prev; }
};

// Auto-Sync
static volatile u64  g_puts_since_sync  = 0;
static volatile bool g_autosync_pending = false;
static constexpr u64 AUTOSYNC_PUT_THRESHOLD = 500;   // ~5 Sekunden intensive Nutzung
static constexpr u32 WAL_WRAP_SAFETY_MARGIN = 4;     // 4 Sektoren vor Wrap triggern

bool wal_in_recovery()        { return g_in_recovery; }
void wal_set_recovery(bool b) { g_in_recovery = b; }
u32  wal_head()               { return g_wal_sector; }
u32  wal_count()              { return g_wal_count; }

static u32 wal_crc(const u8* entries, u32 count) {
    u32 h = 0x811c9dc5;
    u32 n = count * 32;
    for (u32 i = 0; i < n; i++) { h ^= entries[i]; h *= 0x01000193; }
    return h;
}

static void wal_blank_sector() {
    for (int i = 0; i < 512; i++) g_wal_buf[i] = 0;
    *(u32*)(g_wal_buf + 480) = WAL_MAGIC;
    *(u32*)(g_wal_buf + 484) = 0;
    *(u32*)(g_wal_buf + 488) = 0;
    g_wal_count = 0;   // FIX: ohne dies landet der naechste Append falsch
    *(u64*)(g_wal_buf + 492) = g_snap_seq;   // Epoche
}

static void wal_write_current() {
    // WAL ist heilig: direkt auf Disk, nie durch Cache.
    virtio_blk::write_sector(g_wal_sector, g_wal_buf);
}

static void wal_append_internal(u8 op, u8 type, u64 key, u64 a, u64 b) {
    // Wrap-Schutz: rechtzeitig Auto-Sync anfordern, damit der WAL nie
    // bis zum Rand waechst. Der aktuelle Eintrag wird trotzdem geschrieben.
    if (g_wal_sector >= WAL_START + WAL_MAX_SECTORS - WAL_WRAP_SAFETY_MARGIN)
        g_autosync_pending = true;

    if (g_wal_count >= WAL_EPS) {
        g_wal_sector++;
        if (g_wal_sector >= WAL_START + WAL_MAX_SECTORS) {
            // Notfall: Wenn Sync noch nicht gelaufen ist, hart zurueck.
            // Sollte nie passieren, wenn Auto-Sync funktioniert.
            g_wal_sector = WAL_START;
        }
        g_wal_count = 0;
        wal_blank_sector();
    }
    WalEntry* e = (WalEntry*)(g_wal_buf + g_wal_count * 32);
    e->key  = key;
    e->a    = a;
    e->b    = b;
    e->op   = op;
    e->type = type;
    e->_pad = 0;
    e->_pad2 = 0;
    g_wal_count++;
    *(u32*)(g_wal_buf + 484) = g_wal_count;
    *(u32*)(g_wal_buf + 488) = wal_crc(g_wal_buf, g_wal_count);
    wal_write_current();
}

void wal_append_put(u64 key, u32 type, u64 a, u64 b) {
    wal_append_internal(0, (u8)type, key, a, b);
    g_puts_since_sync = g_puts_since_sync + 1;
    if (g_puts_since_sync >= AUTOSYNC_PUT_THRESHOLD)
        g_autosync_pending = true;
}
void wal_append_remove(u64 key) {
    wal_append_internal(1, 0, key, 0, 0);
    g_puts_since_sync = g_puts_since_sync + 1;
    if (g_puts_since_sync >= AUTOSYNC_PUT_THRESHOLD)
        g_autosync_pending = true;
}

// Auto-Sync-API
bool autosync_pending()   { return g_autosync_pending; }
u64  puts_since_sync()    { return g_puts_since_sync; }
void request_autosync()   { g_autosync_pending = true; }

int save();   // forward decl (Definition weiter unten)

int  autosync_now() {
    int r = save();
    return r;
}

using WalApplyFn = void(*)(u8 op, u8 type, u64 key, u64 a, u64 b);
static WalApplyFn g_wal_apply = nullptr;
void wal_set_apply(WalApplyFn fn) { g_wal_apply = fn; }

using WalIdxVisitor = void (*)(u64 index, u8 op, u8 type, u64 key, u64 a, u64 b, void* ctx);

int wal_iterate_indexed(WalIdxVisitor v, void* ctx) {
    u64 idx = 0;
    for (u32 sec = WAL_START; sec <= g_wal_sector; sec++) {
        u8 buf[512];
        if (sec == g_wal_sector) {
            for (int i = 0; i < 512; i++) buf[i] = g_wal_buf[i];
        } else {
            if (virtio_blk::read_sector(sec, buf) != 0) break;
        }
        if (*(u32*)(buf + 480) != WAL_MAGIC) break;
        u32 c = *(u32*)(buf + 484);
        if (c > WAL_EPS) c = WAL_EPS;
        if (*(u32*)(buf + 488) != wal_crc(buf, c)) break;
        if (*(u64*)(buf + 492) != g_snap_seq) break;
        for (u32 i = 0; i < c; i++) {
            WalEntry* e = (WalEntry*)(buf + i * 32);
            v(idx++, e->op, e->type, e->key, e->a, e->b, ctx);
        }
    }
    return 0;
}

int wal_replay() {
    if (!g_wal_apply) return 0;

    // Wir scannen den gesamten WAL-Bereich ab WAL_START.
    // Gueltiger Sektor: MAGIC + CRC passen. Erster Fehler -> Stop.
    u32 last_valid = 0;
    g_in_recovery = true;
    int applied = 0;
    for (u32 sec = WAL_START; sec < WAL_START + WAL_MAX_SECTORS; sec++) {
        u8 buf[512];
        virtio_blk::read_sector(sec, buf);
        u32 magic = *(u32*)(buf + 480);
        if (magic != WAL_MAGIC) break;
        u32 count = *(u32*)(buf + 484);
        if (count > WAL_EPS) break;
        u32 crc = *(u32*)(buf + 488);
        if (crc != wal_crc(buf, count)) break;
        u64 epoche = *(u64*)(buf + 492);
        if (epoche != g_snap_seq) break;   // Sektor einer alten Snapshot-Epoche
        last_valid = sec;
        for (u32 i = 0; i < count; i++) {
            WalEntry* e = (WalEntry*)(buf + i * 32);
            g_wal_apply(e->op, e->type, e->key, e->a, e->b);
            applied++;
        }
    }
    g_in_recovery = false;

    // Naechster Append landet nach dem letzten gueltigen Sektor
    if (last_valid >= WAL_START) {
        g_wal_sector = last_valid;
        u8 buf[512];
        virtio_blk::read_sector(last_valid, buf);
        u32 count = *(u32*)(buf + 484);
        if (count > WAL_EPS) count = WAL_EPS;
        g_wal_count = count;
        for (int i = 0; i < 512; i++) g_wal_buf[i] = buf[i];
    } else {
        wal_blank_sector();
    }
    return applied;
}

void wal_reset() {
    g_wal_sector = WAL_START;
    g_wal_count  = 0;
    wal_blank_sector();
    // WAL ist wie wal_write_current: direkt auf Disk, nie durch den Cache.
    // Sonst ueberlebt ein alter WAL-Sektor ein restore + QEMU-Kill und wird
    // beim naechsten Boot repliziert.
    virtio_blk::write_sector(g_wal_sector, g_wal_buf);
}

// ---------------------------------------------------------------------------
// CHECKPOINTS
// ---------------------------------------------------------------------------
static constexpr u32 CKPT_CATALOG_SECTOR = 8190;
static constexpr u32 CKPT_DATA_START     = 8192;
static constexpr u32 CKPT_CATALOG_MAGIC  = 0x43484B50;
static constexpr u32 CKPT_HEADER_MAGIC   = 0x43484B48;
static constexpr int CKPT_MAX            = 12;

struct __attribute__((packed)) CkptEntry {
    char name[16];
    u64  hash;
    u32  start_sector;
    u32  n_sectors;
    u32  timestamp;
    u32  pad;
};
struct __attribute__((packed)) CkptCatalog {
    u32 magic; u32 count; CkptEntry entries[CKPT_MAX];
};
struct __attribute__((packed)) CkptHeader {
    u32 magic; u32 version;
    u64 hash; u64 snap_seq;
    u32 n_records; u32 n_sectors;
    u32 timestamp; u32 pad;
};

static u32 g_next_ckpt_sector = CKPT_DATA_START;

static void ckpt_name_copy(char* d, const char* src) {
    int i = 0;
    for (; src[i] && i < 15; i++) d[i] = src[i];
    d[i] = 0;
}
static bool ckpt_name_eq(const char* a, const char* b) {
    for (int i = 0; i < 16; i++) {
        if (a[i] != b[i]) return false;
        if (a[i] == 0) return true;
    }
    return true;
}
static int ckpt_catalog_load(CkptCatalog* c) {
    u8 buf[512];
    if (virtio_blk::read_sector(CKPT_CATALOG_SECTOR, buf) != 0) return -1;
    for (int i = 0; i < (int)sizeof(CkptCatalog); i++) ((u8*)c)[i] = buf[i];
    if (c->magic != CKPT_CATALOG_MAGIC) { c->magic = CKPT_CATALOG_MAGIC; c->count = 0; }
    if (c->count > CKPT_MAX) c->count = 0;
    return 0;
}
static int ckpt_catalog_save(const CkptCatalog* c) {
    u8 buf[512];
    for (int i = 0; i < 512; i++) buf[i] = 0;
    for (int i = 0; i < (int)sizeof(CkptCatalog); i++)
        buf[i] = ((const u8*)c)[i];
    return virtio_blk::write_sector(CKPT_CATALOG_SECTOR, buf);
}
static int ckpt_find(const CkptCatalog* c, const char* name) {
    for (u32 i = 0; i < c->count; i++)
        if (ckpt_name_eq(c->entries[i].name, name)) return (int)i;
    return -1;
}

static int ckpt_serialize(u32 start, u32* out_records, u64* out_hash) {
    WriteCtx w;
    for (int i = 0; i < 512; i++) w.buf[i] = 0;
    w.buf_fill = 0; w.sector = start; w.total = 0; w.error = false;
    CollectCtx cc{&w};
    idx::scan_all(collect_visit, &cc);
    flush_partial(&w);
    if (w.error) return -1;

    // Checkpoint-Daten wirklich auf Disk schreiben.
    blockcache::flush();
    if (blockcache::dirty_count() > 0) return -1;

    *out_records = w.total;
    u32 dummy = 0;
    *out_hash = idx::compute_hash(0, 0, &dummy);
    return (int)(w.sector - start);
}

struct TaskSave { u64 key; u64 a; u64 b; };
static TaskSave g_saved_tasks[64];
static int      g_saved_n;

static void save_task_visit(idx::Rec* r, void*) {
    if (g_saved_n >= 64) return;
    g_saved_tasks[g_saved_n].key = r->key;
    g_saved_tasks[g_saved_n].a   = r->a;
    g_saved_tasks[g_saved_n].b   = r->b;
    g_saved_n++;
}

struct CollectRemoveCtx { u64* keys; int* n; int max; };
static void collect_remove_visit(idx::Rec* r, void* v) {
    CollectRemoveCtx* c = (CollectRemoveCtx*)v;
    if (r->type == 0) return;
    if (*(c->n) >= c->max) return;
    c->keys[(*(c->n))++] = r->key;
}

static u64 g_remove_keys[1024];
static int g_remove_n;

struct PersistRec { u64 key; u32 type; u32 _pad; u64 a; u64 b; };
static PersistRec g_backup[512];
static int        g_backup_n;

static void backup_visit(idx::Rec* r, void*) {
    if (g_backup_n >= 512) return;
    if (r->type != 1 && r->type != 4 && r->type != 5 && r->type != 6) return;
    PersistRec& p = g_backup[g_backup_n++];
    p.key = r->key; p.type = r->type; p._pad = 0; p.a = r->a; p.b = r->b;
}

static int ckpt_restore_data(u32 start, u32 n_records, u64 expect_hash) {
    RecoveryGuard guard;

    // 0) Backup des aktuellen persistenten Zustands
    g_backup_n = 0;
    idx::scan_all(backup_visit, nullptr);

    // 1) TASK-Records retten
    g_saved_n = 0;
    idx::scan(idx::Type::TASK, save_task_visit, nullptr);

    // 2) Alle Nicht-TASK-Records entfernen
    g_remove_n = 0;
    CollectRemoveCtx rc{g_remove_keys, &g_remove_n, 1024};
    idx::scan_all(collect_remove_visit, &rc);
    for (int i = 0; i < g_remove_n; i++)
        idx::remove(g_remove_keys[i]);

    // 3) Records aus dem Checkpoint einfuegen
    u32 remaining = n_records;
    u32 sector    = start;
    u8  buf[512];
    while (remaining > 0) {
        if (blockcache::read(sector++, buf) != 0) return -1;
        u32 n = remaining > RECS_PER_SECTOR ? RECS_PER_SECTOR : remaining;
        for (u32 i = 0; i < n; i++) {
            u8* p = buf + i * 32;
            u64 key  = *(u64*)(p + 0);
            u32 type = *(u32*)(p + 8);
            u64 a    = *(u64*)(p + 16);
            u64 b    = *(u64*)(p + 24);
            if (type == 0 || type == 2 || type == 3 || type > 6) continue;
            idx::put(key, (idx::Type)type, a, b);
        }
        remaining -= n;
    }

    // 4) TASK-Records wieder einfuegen
    for (int i = 0; i < g_saved_n; i++)
        idx::put(g_saved_tasks[i].key, idx::Type::TASK,
                 g_saved_tasks[i].a, g_saved_tasks[i].b);

    // 5) WAL sauber machen, neue Epoche
    ++g_snap_seq;
    wal_reset();
    g_puts_since_sync  = 0;
    g_autosync_pending = false;

    // 6) Hash verifizieren
    u32 dummy = 0;
    u64 actual = idx::compute_hash(0, 0, &dummy);
    if (actual == expect_hash) {
        // Bitmap aus dem restaurierten Index rekonstruieren.
        fs::rebuild_bitmap_from_index();
        return 0;
    }

    // ROLLBACK: Checkpoint war unbrauchbar. Index auf Backup zuruecksetzen.
    // 6a) Alles was jetzt drin ist raus
    g_remove_n = 0;
    CollectRemoveCtx rc2{g_remove_keys, &g_remove_n, 1024};
    idx::scan_all(collect_remove_visit, &rc2);
    for (int i = 0; i < g_remove_n; i++)
        idx::remove(g_remove_keys[i]);

    // 6b) Backup wieder einfuegen
    for (int i = 0; i < g_backup_n; i++)
        idx::put(g_backup[i].key, (idx::Type)g_backup[i].type,
                 g_backup[i].a, g_backup[i].b);

    // 6c) TASK-Records wieder rein (die wurden von collect_remove_visit
    //     ausgelassen, sind also noch da — aber sicherheitshalber)
    for (int i = 0; i < g_saved_n; i++)
        idx::put(g_saved_tasks[i].key, idx::Type::TASK,
                 g_saved_tasks[i].a, g_saved_tasks[i].b);

    return -2;
}

int checkpoint_create(const char* name) {
    if (!virtio_blk::is_ready()) return -1;
    if (!name || !*name) return -1;
    CkptCatalog cat;
    ckpt_catalog_load(&cat);
    int existing = ckpt_find(&cat, name);
    u32 start, n_rec_u32; u64 h;
    int n_sec;
    if (existing >= 0) {
        start = cat.entries[existing].start_sector;
        if (ckpt_serialize(start + 1, &n_rec_u32, &h) < 0) return -2;
        n_sec = (int)n_rec_u32 / RECS_PER_SECTOR;
        // Neu berechnen des Werts (oben falsch deklariert)
        u32 nr2 = 0; u64 h2 = 0;
        n_sec = ckpt_serialize(start + 1, &nr2, &h2);
        if (n_sec < 0) return -2;
        n_rec_u32 = nr2; h = h2;
        cat.entries[existing].hash = h;
        cat.entries[existing].n_sectors = (u32)n_sec;
        cat.entries[existing].timestamp = (u32)g_tick;
        ckpt_catalog_save(&cat);
        CkptHeader hdr;
        hdr.magic = CKPT_HEADER_MAGIC; hdr.version = 1;
        hdr.hash = h; hdr.snap_seq = g_snap_seq;
        hdr.n_records = n_rec_u32; hdr.n_sectors = (u32)n_sec;
        hdr.timestamp = (u32)g_tick; hdr.pad = 0;
        u8 hbuf[512];
        for (int i = 0; i < 512; i++) hbuf[i] = 0;
        for (int i = 0; i < (int)sizeof(hdr); i++) hbuf[i] = ((u8*)&hdr)[i];
        return virtio_blk::write_sector(start, hbuf);
    }
    if (cat.count >= CKPT_MAX) return -4;
    start = g_next_ckpt_sector;
    u32 nr = 0; u64 hh = 0;
    n_sec = ckpt_serialize(start + 1, &nr, &hh);
    if (n_sec < 0) return -2;
    CkptHeader hdr;
    hdr.magic = CKPT_HEADER_MAGIC; hdr.version = 1;
    hdr.hash = hh; hdr.snap_seq = g_snap_seq;
    hdr.n_records = nr; hdr.n_sectors = (u32)n_sec;
    hdr.timestamp = (u32)g_tick; hdr.pad = 0;
    u8 hbuf[512];
    for (int i = 0; i < 512; i++) hbuf[i] = 0;
    for (int i = 0; i < (int)sizeof(hdr); i++) hbuf[i] = ((u8*)&hdr)[i];
    if (virtio_blk::write_sector(start, hbuf) != 0) return -3;
    CkptEntry& e = cat.entries[cat.count];
    ckpt_name_copy(e.name, name);
    e.hash = hh;
    e.start_sector = start;
    e.n_sectors = (u32)n_sec;
    e.timestamp = (u32)g_tick;
    e.pad = 0;
    cat.count++;
    ckpt_catalog_save(&cat);
    g_next_ckpt_sector += 1 + (u32)n_sec;
    return 0;
}

int checkpoint_restore(const char* name) {
    if (!virtio_blk::is_ready()) return -1;
    CkptCatalog cat;
    ckpt_catalog_load(&cat);
    int i = ckpt_find(&cat, name);
    if (i < 0) return -2;
    CkptEntry& e = cat.entries[i];
    u8 hbuf[512];
    if (blockcache::read(e.start_sector, hbuf) != 0) return -3;
    CkptHeader hdr;
    for (int k = 0; k < (int)sizeof(hdr); k++) ((u8*)&hdr)[k] = hbuf[k];
    if (hdr.magic != CKPT_HEADER_MAGIC) return -4;
    return ckpt_restore_data(e.start_sector + 1, hdr.n_records, hdr.hash);
}

int checkpoint_count() {
    CkptCatalog cat;
    ckpt_catalog_load(&cat);
    return (int)cat.count;
}

int save() {
    if (!virtio_blk::is_ready()) return -1;

    WriteCtx w;
    for (int i = 0; i < 512; i++) w.buf[i] = 0;
    w.buf_fill = 0;
    w.sector   = (u32)SNAP_START;
    w.total    = 0;
    w.error    = false;

    CollectCtx c{&w};
    idx::scan_all(collect_visit, &c);
    flush_partial(&w);

    if (w.error) return -2;

    blockcache::flush();
    if (blockcache::dirty_count() > 0) return -4;

    ++g_snap_seq;
    Superblock sb;
    const char* m = "NEXOS___";
    for (int i = 0; i < 8; i++) sb.magic[i] = m[i];
    sb.version    = 1;
    sb.pad0       = 0;
    sb.snap_seq   = g_snap_seq;
    sb.snap_start = (u32)SNAP_START;
    sb.snap_count = w.total;
    sb.wal_start  = WAL_START;
    sb.wal_head   = WAL_START;
    sb.wal_tail   = WAL_START;
    sb.pad1       = 0;
    sb.crc        = sb_crc((const u8*)&sb, 48);

    u8 sb_buf[512];
    for (int i = 0; i < 512; i++) sb_buf[i] = 0;
    for (int i = 0; i < (int)sizeof(sb); i++) sb_buf[i] = ((u8*)&sb)[i];
    if (blockcache::write(0, sb_buf) != 0) return -3;
    if (blockcache::write(1, sb_buf) != 0) return -3;

    blockcache::flush();
    if (blockcache::dirty_count() > 0) return -4;

    wal_reset();
    g_puts_since_sync  = 0;
    g_autosync_pending = false;

    return (int)w.total;
}

int load() {
    if (!virtio_blk::is_ready()) return -1;

    u8 sb_buf[512];
    if (blockcache::read(0, sb_buf) != 0) return -3;
    Superblock sb;
    for (int i = 0; i < (int)sizeof(sb); i++) ((u8*)&sb)[i] = sb_buf[i];

    const char* m = "NEXOS___";
    for (int i = 0; i < 8; i++)
        if (sb.magic[i] != m[i]) return 0;

    u32 c = sb_crc((const u8*)&sb, 48);
    if (c != sb.crc) return -2;

    g_snap_seq = sb.snap_seq;
    g_wal_head_saved = sb.wal_head;
    g_wal_sector = sb.wal_head;
    g_wal_count  = 0;
    RecoveryGuard guard;

    u32 remaining = sb.snap_count;
    u32 sector    = sb.snap_start;
    u8  buf[512];
    while (remaining > 0) {
        if (blockcache::read(sector++, buf) != 0) return -3;
        u32 n = remaining > RECS_PER_SECTOR ? RECS_PER_SECTOR : remaining;
        for (u32 i = 0; i < n; i++) {
            u8* p = buf + i * 32;
            u64 key  = *(u64*)(p + 0);
            u32 type = *(u32*)(p + 8);
            u64 a    = *(u64*)(p + 16);
            u64 b    = *(u64*)(p + 24);
            if (type == 0 || type == 2 || type == 3 || type > 6) continue;
            idx::put(key, (idx::Type)type, a, b);
        }
        remaining -= n;
    }
    return (int)sb.snap_count;
}

u64 snap_seq() { return g_snap_seq; }

}  // namespace persist

// C-API
extern "C" void nexus_wal_put(u64 key, u32 type, u64 a, u64 b) {
    // Persistente Typen: CONFIG=1, SYS=4, FILE=5, NAME=6
    // Fluechtig: TASK=0, EVENT=2, TRIGGER=3
    if (type == 0 || type == 2 || type == 3) return;
    persist::wal_append_put(key, type, a, b);
}
extern "C" void nexus_wal_remove(u64 key) {
    u64 ns = key & 0xFF00000000000000ULL;
    if (ns == (0x02ULL << 56) || ns == (0x03ULL << 56)) return;  // TASK/EVENT
    persist::wal_append_remove(key);
}
extern "C" bool nexus_wal_in_recovery() {
    return persist::wal_in_recovery();
}

// fs (bitmap, block-alloc, write_file, cat_file) moved to kernel/fs/

