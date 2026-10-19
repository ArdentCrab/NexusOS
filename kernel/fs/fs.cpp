// kernel/fs/fs.cpp — Refactor Session 5
#include "kernel/fs/fs.h"
#include "kernel/idx/idx.h"

// Forward-Decls: dev/ (S7) noch nicht extrahiert.
// Diese verschwinden, sobald S7 kernel/dev/ liefert.
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

// ---------------------------------------------------------------------------
// FS — Index-Native File-Records
// ---------------------------------------------------------------------------
namespace fs {

static constexpr u64 BITMAP_START   = 2048;
static constexpr u64 BITMAP_SECTORS = 32;
static constexpr u64 DATA_START     = 4096;
static constexpr u32 EXTENTS_PER_INDEX  = 60;   // 60 * 8 = 480 B im 512-B-Sektor
static constexpr u32 INDEX_CHAIN_MAX    = 256;  // Sicherheits-Cap gegen korrupte next-Ketten
static constexpr u32 EXTENT_MAX_SECTORS = 0x1000;  // max 2 MiB pro Extent

struct __attribute__((packed)) FsExtent {
    u32 start;   // LBA
    u32 count;   // Anzahl zusammenhaengender Sektoren
};
struct __attribute__((packed)) FsIndex {
    u64 next;                        // 0 = Ende, sonst LBA des naechsten Index-Sektors
    u32 n_extents;                   // 0..EXTENTS_PER_INDEX
    u32 _pad;
    FsExtent extents[EXTENTS_PER_INDEX];
};
static_assert(sizeof(FsIndex) <= 512, "FsIndex muss in einen Sektor passen");

static u8  g_bitmap[BITMAP_SECTORS * 512];
static u8  g_bitmap_dirty[BITMAP_SECTORS];
static u64 g_bm_writes = 0;   // Diagnose: tatsaechliche Sektor-Writes
static bool g_bitmap_loaded = false;

u64 hash_name(const char* s) {
    u64 h = 5381;
    while (*s) { h = ((h << 5) + h) + (u8)*s; s++; }
    return h & 0x00FFFFFFFFFFFFFFULL;
}

static inline bool blk_test(u64 sector) {
    u64 idx = sector / 8;
    if (idx >= sizeof(g_bitmap)) return true;
    return (g_bitmap[idx] >> (sector & 7)) & 1;
}
// sector = LBA eines Datenblocks. Bit-Index = sector.
// Byte-Index = sector/8. Bitmap-Sektor = Byte-Index / 512.
static inline void blk_mark_dirty(u64 sector) {
    u64 bm_sec = (sector / 8) / 512;
    if (bm_sec < BITMAP_SECTORS) g_bitmap_dirty[bm_sec] = 1;
}
static inline void blk_set(u64 sector) {
    u64 idx = sector / 8;
    if (idx >= sizeof(g_bitmap)) return;
    g_bitmap[idx] |= (u8)(1u << (sector & 7));
    blk_mark_dirty(sector);
}
static inline void blk_clear(u64 sector) {
    u64 idx = sector / 8;
    if (idx >= sizeof(g_bitmap)) return;
    g_bitmap[idx] &= (u8)~(1u << (sector & 7));
    blk_mark_dirty(sector);
}

void bitmap_load() {
    if (g_bitmap_loaded) return;
    if (!virtio_blk::is_ready()) return;
    for (u64 i = 0; i < BITMAP_SECTORS; i++) {
        virtio_blk::read_sector(BITMAP_START + i, g_bitmap + i * 512);
        g_bitmap_dirty[i] = 0;
    }
    g_bitmap_loaded = true;
}
void bitmap_save() {
    if (!virtio_blk::is_ready()) return;
    for (u64 i = 0; i < BITMAP_SECTORS; i++) {
        if (!g_bitmap_dirty[i]) continue;
        int r = virtio_blk::write_sector(BITMAP_START + i, g_bitmap + i * 512);
        if (r == 0) {
            g_bitmap_dirty[i] = 0;   // nur bei Erfolg loeschen
            g_bm_writes++;
        }
        // bei Fehler: dirty bleibt gesetzt, naechster save versucht es erneut
    }
}

// Blk-Alloc ohne Save (Batch-Alloc), Save am Ende
u64 blk_alloc_nosave() {
    bitmap_load();
    u64 cap = virtio_blk::capacity();
    for (u64 sec = DATA_START; sec < cap; sec++) {
        if (!blk_test(sec)) { blk_set(sec); return sec; }
    }
    return 0;
}
static u64 blk_alloc() {
    return blk_alloc_nosave();   // Save erfolgt am Ende der FS-Operation
}
// Sucht n aufeinanderfolgende freie Sektoren, markiert sie, gibt Start-LBA zurueck.
// 0 = kein zusammenhaengender Lauf dieser Groesse verfuegbar.
static u64 blk_alloc_run(u32 n) {
    if (n == 0) return 0;
    if (n == 1) return blk_alloc_nosave();
    bitmap_load();
    u64 cap = virtio_blk::capacity();
    u64 run = 0;
    for (u64 sec = DATA_START; sec < cap; sec++) {
        if (!blk_test(sec)) {
            if (++run == n) {
                u64 base = sec + 1 - n;
                for (u64 i = base; i <= sec; i++) blk_set(i);
                return base;
            }
        } else {
            run = 0;
        }
    }
    return 0;
}
u64 bm_write_count() { return g_bm_writes; }
int bm_dirty_count() {
    int n = 0;
    for (u64 i = 0; i < BITMAP_SECTORS; i++) if (g_bitmap_dirty[i]) n++;
    return n;
}
void blk_free(u64 sector) {
    if (sector < DATA_START) return;
    blk_clear(sector);           // markiert nur dirty
    // Save erfolgt am Ende der FS-Operation
}
// Extent-Chain freigeben: alle Datenbloecke + alle Index-Sektoren.
static void fs_release_chain(u64 first_idx) {
    u64 idx_sec = first_idx;
    u32 chain = 0;
    while (idx_sec && chain++ < INDEX_CHAIN_MAX) {
        u8 ibuf[512];
        virtio_blk::read_sector(idx_sec, ibuf);
        FsIndex* ix = (FsIndex*)ibuf;
        if (ix->n_extents > EXTENTS_PER_INDEX) break;
        for (u32 e = 0; e < ix->n_extents; e++) {
            u32 st = ix->extents[e].start;
            u32 cn = ix->extents[e].count;
            for (u32 s = 0; s < cn; s++) blk_free(st + s);
        }
        u64 next = ix->next;
        blk_free(idx_sec);
        idx_sec = next;
    }
}
// Datei-Ref freigeben (inline = nichts, Einzelsektor = ein Block, sonst Chain).
static void fs_release_file(u64 size, u64 ref) {
    if (size <= 8)        return;
    if (size <= 512)      { blk_free(ref); return; }
    fs_release_chain(ref);
}

// Sektoren einer Datei in der Bitmap markieren.
static void mark_one_file(u64 size, u64 ref) {
    if (size <= 8) return;
    if (size <= 512) { blk_set(ref); return; }
    u64 idx_sec = ref;
    u32 chain = 0;
    while (idx_sec && chain++ < INDEX_CHAIN_MAX) {
        blk_set(idx_sec);
        u8 ibuf[512];
        virtio_blk::read_sector(idx_sec, ibuf);
        FsIndex* ix = (FsIndex*)ibuf;
        if (ix->n_extents > EXTENTS_PER_INDEX) break;
        for (u32 e = 0; e < ix->n_extents; e++) {
            u32 st = ix->extents[e].start;
            u32 cn = ix->extents[e].count;
            for (u32 k = 0; k < cn; k++) blk_set(st + k);
        }
        idx_sec = ix->next;
    }
}
static void rebuild_visit(idx::Rec* r, void*) {
    if (r->type != (u32)idx::Type::FILE) return;
    mark_one_file(r->a, r->b);
}
// Bitmap aus dem Index rekonstruieren. Wird nach load/restore gerufen,
// weil die Bitmap nicht Teil des Snapshots ist. Ohne Rebuild vergibt der
// Allocator nach einem restore Sektoren, die noch an einen FILE-Record
// gebunden sind.
u64 used_data_sectors() {
    u64 cap = virtio_blk::capacity();
    u64 n = 0;
    for (u64 sec = DATA_START; sec < cap; sec++) {
        if (blk_test(sec)) n++;
    }
    return n;
}
void rebuild_bitmap_from_index() {
    if (!virtio_blk::is_ready()) return;
    u64 cap = virtio_blk::capacity();
    // Alle Daten-Sektoren als frei markieren.
    for (u64 sec = DATA_START; sec < cap; sec++) {
        u64 bidx = sec / 8;
        if (bidx >= sizeof(g_bitmap)) break;
        u8 bit = (u8)(1u << (sec & 7));
        if (g_bitmap[bidx] & bit) {
            g_bitmap[bidx] &= (u8)~bit;
            blk_mark_dirty(sec);
        }
    }
    // Aus allen FILE-Records neu markieren.
    idx::scan(idx::Type::FILE, rebuild_visit, nullptr);
    bitmap_save();
}

// ---- File-Write mit Extent-Chaining ----
int write_file(const char* name, const char* content, usize len) {
    if (!name || !*name) return -1;
    usize nlen = 0; while (name[nlen] && nlen < 16) nlen++;

    u64 h = hash_name(name);
    u64 fkey = idx::NS_FILE | h;
    u64 nkey = idx::NS_NAME | h;

    u64 na = 0, nb = 0;
    for (usize i = 0; i < 8 && i < nlen; i++) na |= ((u64)(u8)name[i]) << (i * 8);
    for (usize i = 8; i < 16 && i < nlen; i++) nb |= ((u64)(u8)name[i]) << ((i - 8) * 8);
    idx::put(nkey, idx::Type::NAME, na, nb);

    // 1) Alte Werte merken, NICHT freigeben
    u64 old_size = 0, old_ref = 0;
    bool has_old = false;
    idx::Rec* old = idx::find(fkey);
    if (old && old->type == (u32)idx::Type::FILE) {
        old_size = old->a;
        old_ref  = old->b;
        has_old  = true;
    }

    // 2) Neue Daten schreiben
    u64 a_val = (u64)len;
    u64 b_val = 0;

    if (len <= 8) {
        for (usize i = 0; i < len; i++)
            b_val |= ((u64)(u8)content[i]) << (i * 8);
    } else if (len <= 512) {
        if (!virtio_blk::is_ready()) return -2;
        u64 sec = blk_alloc_nosave();
        if (!sec) { bitmap_save(); return -3; }
        u8 buf[512];
        for (int i = 0; i < 512; i++) buf[i] = 0;
        for (usize i = 0; i < len; i++) buf[i] = (u8)content[i];
        virtio_blk::write_sector(sec, buf);
        b_val = sec;
    } else {
        if (!virtio_blk::is_ready()) return -2;
        u64 total_sectors = (len + 511) / 512;

        // Ersten Index-Sektor allokieren
        u64 first_idx_sec = blk_alloc_nosave();
        if (!first_idx_sec) { bitmap_save(); return -3; }

        u64 cur_idx_sec = first_idx_sec;
        FsIndex cur_idx;
        for (u32 i = 0; i < sizeof(FsIndex); i++) ((u8*)&cur_idx)[i] = 0;
        u32 chain = 1;
        u64 written = 0;

        auto flush_idx = [](u64 sec, FsIndex* ix) {
            u8 ibuf[512];
            for (int i = 0; i < 512; i++) ibuf[i] = 0;
            for (u32 k = 0; k < sizeof(FsIndex); k++) ibuf[k] = ((u8*)ix)[k];
            virtio_blk::write_sector(sec, ibuf);
        };

        while (written < total_sectors) {
            if (cur_idx.n_extents >= EXTENTS_PER_INDEX) {
                flush_idx(cur_idx_sec, &cur_idx);
                u64 next_sec = blk_alloc_nosave();
                if (!next_sec) { bitmap_save(); return -3; }
                cur_idx_sec = next_sec;
                for (u32 i = 0; i < sizeof(FsIndex); i++) ((u8*)&cur_idx)[i] = 0;
                if (++chain > INDEX_CHAIN_MAX) { bitmap_save(); return -5; }
            }

            u64 remain = total_sectors - written;
            u32 want = (remain > EXTENT_MAX_SECTORS) ? EXTENT_MAX_SECTORS : (u32)remain;
            u64 data_lba = blk_alloc_run(want);
            if (!data_lba) {
                // fragmentiert: Stueck fuer Stueck
                data_lba = blk_alloc_nosave();
                if (!data_lba) { bitmap_save(); return -3; }
                want = 1;
            }
            FsExtent& ex = cur_idx.extents[cur_idx.n_extents++];
            ex.start = (u32)data_lba;
            ex.count = want;

            for (u32 s = 0; s < want; s++) {
                u8 dbuf[512];
                for (int j = 0; j < 512; j++) dbuf[j] = 0;
                u64 off = (written + s) * 512;
                usize chunk = (len - off > 512) ? 512 : (len - off);
                for (usize j = 0; j < chunk; j++) dbuf[j] = (u8)content[off + j];
                virtio_blk::write_sector(data_lba + s, dbuf);
            }
            written += want;
        }
        flush_idx(cur_idx_sec, &cur_idx);
        b_val = first_idx_sec;
    }

    // 3) Bitmap VOR idx::put speichern
    bitmap_save();

    // 4) File-Record umhaengen
    idx::put(fkey, idx::Type::FILE, a_val, b_val);

    // 5) Erst jetzt alte Bloecke freigeben
    if (has_old && old_size > 8) {
        fs_release_file(old_size, old_ref);
        bitmap_save();
    }

    return (int)len;
}

// KNOWN LIMITATION (P2): laedt immer die komplette Datei in den Puffer.
// Bei Dateien >1 MiB oder vielen gleichzeitig offenen fds wird der 8 MiB
// Heap knapp. Spaeter: extent-aware read_at(offset, len).
int cat_file(const char* name, char* out, usize out_max) {
    u64 h = hash_name(name);
    u64 fkey = idx::NS_FILE | h;
    idx::Rec* r = idx::find(fkey);
    if (!r || r->type != (u32)idx::Type::FILE) return -1;

    u64 len = r->a;
    u64 b_val = r->b;
    usize n = (usize)len;
    if (n > out_max - 1) n = out_max - 1;

    if (len <= 8) {
        for (usize i = 0; i < n; i++) out[i] = (char)((b_val >> (i * 8)) & 0xFF);
    } else if (len <= 512) {
        u8 buf[512];
        virtio_blk::read_sector(b_val, buf);
        for (usize i = 0; i < n; i++) out[i] = (char)buf[i];
    } else {
        u64 idx_sec = b_val;
        usize copied = 0;
        u32 chain = 0;
        while (idx_sec && copied < n && chain++ < INDEX_CHAIN_MAX) {
            u8 ibuf[512];
            virtio_blk::read_sector(idx_sec, ibuf);
            FsIndex* ix = (FsIndex*)ibuf;
            if (ix->n_extents > EXTENTS_PER_INDEX) break;
            for (u32 e = 0; e < ix->n_extents && copied < n; e++) {
                u32 st = ix->extents[e].start;
                u32 cn = ix->extents[e].count;
                for (u32 s = 0; s < cn && copied < n; s++) {
                    u8 dbuf[512];
                    virtio_blk::read_sector(st + s, dbuf);
                    usize chunk = n - copied;
                    if (chunk > 512) chunk = 512;
                    for (usize j = 0; j < chunk; j++) out[copied + j] = (char)dbuf[j];
                    copied += chunk;
                }
            }
            idx_sec = ix->next;
        }
    }
    out[n] = 0;
    return (int)len;
}

struct ListCtx { int count; };
static void file_visit(idx::Rec* r, void* ctx) {
    ListCtx* c = (ListCtx*)ctx;
    c->count++;
    u64 h = r->key & 0x00FFFFFFFFFFFFFFULL;
    idx::Rec* nr = idx::find(idx::NS_NAME | h);
    char nm[20];
    int nn = 0;
    if (nr) {
        for (int i = 0; i < 8; i++) {
            char ch = (char)((nr->a >> (i * 8)) & 0xFF);
            if (!ch) break;
            nm[nn++] = ch;
        }
        for (int i = 0; i < 8; i++) {
            char ch = (char)((nr->b >> (i * 8)) & 0xFF);
            if (!ch) break;
            nm[nn++] = ch;
        }
    }
    nm[nn] = 0;
    hal::ps("  ");
    if (nn > 0) { hal::ps(nm); int pad = 16 - nn; while (pad-- > 0) hal::pc(' '); }
    else        { hal::ps("?               "); }
    hal::pf("size=%u  ref=", (u32)r->a);
    if (r->a <= 8)       hal::ps("(inline)");
    else if (r->a <= 512) hal::pf("sect@%p", r->b);
    else                  hal::pf("chain@%p", r->b);
    hal::pc('\n');
}

void ls_files() {
    hal::ps("files:\n");
    ListCtx c{0};
    idx::scan(idx::Type::FILE, file_visit, &c);
    hal::pf("  total: %u\n", c.count);
}

int rm_file(const char* name) {
    u64 h = hash_name(name);
    u64 fkey = idx::NS_FILE | h;
    u64 nkey = idx::NS_NAME | h;
    idx::Rec* r = idx::find(fkey);
    if (!r || r->type != (u32)idx::Type::FILE) return -1;

    u64 size = r->a;
    u64 ref  = r->b;
    if (size > 8) {
        if (size <= 512) {
            blk_free(ref);
        } else {
            fs_release_chain(ref);
        }
    }

    idx::remove(fkey);
    idx::remove(nkey);
    bitmap_save();   // Batch-Save am Ende der Operation
    return 0;
}

}  // namespace fs

namespace fs {
i64 parse_path(const char* path, Path* out) {
  if (!path || !out) return FS_E_INVAL;
  if (*path != '/') return FS_E_INVAL;
  u32 pl = 0; while (path[pl]) pl++;
  if (pl > 1 && path[pl-1] == '/') return FS_E_INVAL;
  out->n = 0; const char* p = path + 1;
  if (!*p) return 0;
  while (*p) {
    if (out->n >= PATH_MAX_DEPTH) return FS_E_NAMETOOLONG;
    u32 len = 0;
    while (*p && *p != '/') {
      if (len >= PATH_MAX_NAME - 1) return FS_E_NAMETOOLONG;
      out->parts[out->n][len++] = *p++;
    }
    if (len == 0) return FS_E_INVAL;
    out->parts[out->n][len] = 0; out->n++;
    if (*p == '/') p++;
  }
  return 0;
}
i64 build_path(const Path* p, char* out, u32 out_max) {
  if (!p || !out || out_max < 2) return FS_E_INVAL;
  u32 pos = 0; out[pos++] = '/';
  if (p->n == 0) { out[pos] = 0; return (i64)pos; }
  for (u32 i = 0; i < p->n; i++) {
    u32 len = 0; while (p->parts[i][len]) len++;
    if (pos + len + 1 > out_max) return FS_E_NAMETOOLONG;
    for (u32 k = 0; k < len; k++) out[pos++] = p->parts[i][k];
    if (i + 1 < p->n) out[pos++] = '/';
  }
  out[pos] = 0; return (i64)pos;
}
i64 dirname_of(const char* path, char* out, u32 out_max) {
  Path p; i64 r = parse_path(path, &p);
  if (r < 0) return r;
  if (p.n <= 1) { if (out_max < 2) return FS_E_NAMETOOLONG; out[0]='/'; out[1]=0; return 1; }
  p.n--; return build_path(&p, out, out_max);
}
i64 basename_of(const char* path, char* out, u32 out_max) {
  Path p; i64 r = parse_path(path, &p);
  if (r < 0) return r;
  if (p.n == 0) { if (out_max < 2) return FS_E_NAMETOOLONG; out[0]='/'; out[1]=0; return 1; }
  const char* nm = p.parts[p.n-1];
  u32 len = 0; while (nm[len]) len++;
  if (len + 1 > out_max) return FS_E_NAMETOOLONG;
  for (u32 i = 0; i < len; i++) out[i] = nm[i];
  out[len] = 0; return (i64)len;
}
} // namespace fs (A7a)

namespace fs {

bool dir_exists(const char* path) {
    Path p;
    if (parse_path(path, &p) < 0) return false;
    char norm[256];
    if (build_path(&p, norm, sizeof(norm)) < 0) return false;
    u64 h = hash_name(norm);
    idx::Rec* r = idx::find(idx::NS_DIR | h);
    return r && r->type == (u32)idx::Type::DIR;
}

i64 fs_mkdir(const char* path) {
    Path p;
    if (parse_path(path, &p) < 0) return FS_E_INVAL;
    if (p.n == 0) return FS_E_INVAL;
    char norm[256];
    if (build_path(&p, norm, sizeof(norm)) < 0) return FS_E_NAMETOOLONG;
    if (dir_exists(norm)) return -17;
    for (u32 k = 1; k < p.n; k++) {
        Path par; par.n = k;
        for (u32 i = 0; i < k; i++) {
            u32 j = 0; while (p.parts[i][j]) { par.parts[i][j] = p.parts[i][j]; j++; }
            par.parts[i][j] = 0;
        }
        char pbuf[256];
        if (build_path(&par, pbuf, sizeof(pbuf)) < 0) return FS_E_NAMETOOLONG;
        if (!dir_exists(pbuf)) return -2;
    }
    u64 h = hash_name(norm);
    idx::put(idx::NS_DIR | h, idx::Type::DIR, 0, 0);
    return 0;
}

// A7b-1: prüft noch NICHT ob Verzeichnis leer ist (kommt in A7b-2)
i64 fs_rmdir(const char* path) {
    Path p;
    if (parse_path(path, &p) < 0) return FS_E_INVAL;
    if (p.n == 0) return FS_E_INVAL;
    char norm[256];
    if (build_path(&p, norm, sizeof(norm)) < 0) return FS_E_NAMETOOLONG;
    if (!dir_exists(norm)) return -2;
    u64 h = hash_name(norm);
    idx::remove(idx::NS_DIR | h);
    return 0;
}

} // namespace fs (A7b-1)
