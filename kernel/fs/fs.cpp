// kernel/fs/fs.cpp — Refactor Session 5
#include "kernel/fs/fs.h"
#include "kernel/task/task.h"
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
  char abs[256];
  const char* cwd = task::current ? task::current->cwd : "/";
  if (fs_resolve(name, cwd, abs, sizeof(abs)) < 0) return -22;
  return (int)fs_write_abs(abs, content, len);
}

// A7b-3a: absolute write. 3-Record-Update (FILE + NAME + PATH).
i64 fs_write_abs(const char* abs, const char* content, usize len) {
  u64 h = hash_name(abs[0] == '/' ? abs + 1 : abs);
  u64 fkey = idx::NS_FILE | h;
  u64 nkey = idx::NS_NAME | h;
  u64 pkey = idx::NS_PATH | h;

  // basename + parent
  char bn[32]; char par[256];
  if (basename_of(abs, bn, sizeof(bn)) < 0) return -22;
  u64 parent_hash = 0;
  if (dirname_of(abs, par, sizeof(par)) > 0) parent_hash = hash_name(par);

  // NAME: a=basename[0..8], b=parent_hash
  u64 na = 0;
  for (int i = 0; i < 8 && bn[i]; i++) na |= ((u64)(u8)bn[i]) << (i*8);
  idx::put(nkey, idx::Type::NAME, na, parent_hash);

  // PATH: a=parent_hash, b=0
  idx::put(pkey, idx::Type::PATH, parent_hash, 0);

  // alter FILE-Record
  u64 old_size = 0, old_ref = 0; bool has_old = false;
  idx::Rec* old = idx::find(fkey);
  if (old && old->type == (u32)idx::Type::FILE) {
    old_size = old->a; old_ref = old->b; has_old = true;
  }

  u64 a_val = (u64)len, b_val = 0;
  if (len <= 8) {
    for (usize i = 0; i < len; i++) b_val |= ((u64)(u8)content[i]) << (i*8);
  } else if (len <= 512) {
    if (!virtio_blk::is_ready()) return -2;
    u64 sec = blk_alloc_nosave();
    if (!sec) { bitmap_save(); return -3; }
    u8 buf[512]; for (int i = 0; i < 512; i++) buf[i] = 0;
    for (usize i = 0; i < len; i++) buf[i] = (u8)content[i];
    virtio_blk::write_sector(sec, buf); b_val = sec;
  } else {
    if (!virtio_blk::is_ready()) return -2;
    u64 total_sectors = (len + 511) / 512;
    u64 first_idx_sec = blk_alloc_nosave();
    if (!first_idx_sec) { bitmap_save(); return -3; }
    u64 cur_idx_sec = first_idx_sec;
    FsIndex cur_idx; for (u32 i = 0; i < sizeof(FsIndex); i++) ((u8*)&cur_idx)[i] = 0;
    u32 chain = 1; u64 written = 0;
    auto flush_idx = [](u64 sec, FsIndex* ix) {
      u8 ibuf[512]; for (int i = 0; i < 512; i++) ibuf[i] = 0;
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
        data_lba = blk_alloc_nosave();
        if (!data_lba) { bitmap_save(); return -3; }
        want = 1;
      }
      FsExtent& ex = cur_idx.extents[cur_idx.n_extents++];
      ex.start = (u32)data_lba; ex.count = want;
      for (u32 si = 0; si < want; si++) {
        u8 dbuf[512]; for (int j = 0; j < 512; j++) dbuf[j] = 0;
        u64 off = (written + si) * 512;
        usize chunk = (len - off > 512) ? 512 : (len - off);
        for (usize j = 0; j < chunk; j++) dbuf[j] = (u8)content[off + j];
        virtio_blk::write_sector(data_lba + si, dbuf);
      }
      written += want;
    }
    flush_idx(cur_idx_sec, &cur_idx);
    b_val = first_idx_sec;
  }
  bitmap_save();
  idx::put(fkey, idx::Type::FILE, a_val, b_val);
  if (has_old && old_size > 8) { fs_release_file(old_size, old_ref); bitmap_save(); }
  return (i64)len;
}

// KNOWN LIMITATION (P2): laedt immer die komplette Datei in den Puffer.
// Bei Dateien >1 MiB oder vielen gleichzeitig offenen fds wird der 8 MiB
// Heap knapp. Spaeter: extent-aware read_at(offset, len).
int cat_file(const char* name, char* out, usize out_max) {
  char abs[256];
  const char* cwd = task::current ? task::current->cwd : "/";
  if (fs_resolve(name, cwd, abs, sizeof(abs)) < 0) return -1;
  i64 r = fs_cat_abs(abs, out, out_max);
  return (int)r;
}

// A7b-3a: absolute cat.
i64 fs_cat_abs(const char* abs, char* out, usize out_max) {
  u64 h = hash_name(abs[0] == '/' ? abs + 1 : abs);
  u64 fkey = idx::NS_FILE | h;
  idx::Rec* r = idx::find(fkey);
  if (!r || r->type != (u32)idx::Type::FILE) return -1;
  u64 len = r->a; u64 b_val = r->b;
  usize n = (usize)len;
  if (n > out_max - 1) n = out_max - 1;
  if (len <= 8) {
    for (usize i = 0; i < n; i++) out[i] = (char)((b_val >> (i*8)) & 0xFF);
  } else if (len <= 512) {
    u8 buf[512]; virtio_blk::read_sector(b_val, buf);
    for (usize i = 0; i < n; i++) out[i] = (char)buf[i];
  } else {
    u64 idx_sec = b_val; usize copied = 0; u32 chain = 0;
    while (idx_sec && copied < n && chain++ < INDEX_CHAIN_MAX) {
      u8 ibuf[512]; virtio_blk::read_sector(idx_sec, ibuf);
      FsIndex* ix = (FsIndex*)ibuf;
      if (ix->n_extents > EXTENTS_PER_INDEX) break;
      for (u32 e = 0; e < ix->n_extents && copied < n; e++) {
        u32 st = ix->extents[e].start; u32 cn = ix->extents[e].count;
        for (u32 si = 0; si < cn && copied < n; si++) {
          u8 dbuf[512]; virtio_blk::read_sector(st + si, dbuf);
          usize chunk = n - copied; if (chunk > 512) chunk = 512;
          for (usize j = 0; j < chunk; j++) out[copied + j] = (char)dbuf[j];
          copied += chunk;
        }
      }
      idx_sec = ix->next;
    }
  }
  out[n] = 0;
  return (i64)len;
}

// A7b-3a: Migration. Fuer alle NS_NAME-Records, die noch alt-Format haben
// (b = basename[8..16] statt parent_hash), setze b = hash("/") = 177620.
// Da alle Alt-Files in Root lagen, ist Parent = "/".
static void _migrate_name_visit(idx::Rec* r, void*) {
  u64 root_hash = hash_name("/");
  if (r->b != root_hash) idx::put(r->key, idx::Type::NAME, r->a, root_hash);
}
static void _migrate_file_visit(idx::Rec* r, void*) {
  u64 h = r->key & 0x00FFFFFFFFFFFFFFULL;
  if (!idx::find(idx::NS_PATH | h)) {
    u64 root_hash = hash_name("/");
    idx::put(idx::NS_PATH | h, idx::Type::PATH, root_hash, 0);
  }
}
void migrate_files_v2() {
  idx::scan(idx::Type::NAME, _migrate_name_visit, nullptr);
  idx::scan(idx::Type::FILE, _migrate_file_visit, nullptr);
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
  struct Ctx { int count; } c{0};
  auto visit = [](idx::Rec* r, void* v) {
    Ctx* cc = (Ctx*)v; cc->count++;
    u64 h = r->key & 0x00FFFFFFFFFFFFFFULL;
    idx::Rec* nr = idx::find(idx::NS_NAME | h);
    char nm[20]; int nn = 0;
    if (nr) {
      for (int i = 0; i < 8; i++) {
        char ch = (char)((nr->a >> (i*8)) & 0xFF);
        if (!ch) break;
        nm[nn++] = ch;
      }
    }
    nm[nn] = 0;
    hal::ps("  ");
    if (nn > 0) { hal::ps(nm); int pad = 16 - nn; while (pad-- > 0) hal::pc(' '); }
    else        { hal::ps("?               "); }
    hal::pf("size=%u  ref=", (u32)r->a);
    if (r->a <= 8) hal::ps("(inline)");
    else if (r->a <= 512) hal::pf("sect@%p", r->b);
    else hal::pf("chain@%p", r->b);
    hal::pc('\n');
  };
  hal::ps("files:\n");
  idx::scan(idx::Type::FILE, visit, &c);
  hal::pf("  total: %u\n", c.count);
}

int rm_file(const char* name) {
  char abs[256];
  const char* cwd = task::current ? task::current->cwd : "/";
  if (fs_resolve(name, cwd, abs, sizeof(abs)) < 0) return -22;
  return (int)fs_rm_abs(abs);
}

i64 fs_rm_abs(const char* abs) {
  u64 h = hash_name(abs[0] == '/' ? abs + 1 : abs);
  u64 fkey = idx::NS_FILE | h;
  idx::Rec* r = idx::find(fkey);
  if (!r || r->type != (u32)idx::Type::FILE) return -2;
  u64 size = r->a, ref = r->b;
  if (size > 8) {
    if (size <= 512) blk_free(ref);
    else fs_release_chain(ref);
  }
  idx::remove(fkey);
  idx::remove(idx::NS_NAME | h);
  idx::remove(idx::NS_PATH | h);
  bitmap_save();
  return 0;
}

}  // namespace fs

namespace fs {
i64 parse_path(const char* path, Path* out) {
  if (!path || !out) return FS_E_INVAL;
  if (*path != '/') return FS_E_INVAL;
  out->n = 0;
  const char* p = path + 1;
  if (!*p) return 0;
  while (*p) {
    u32 len = 0;
    while (*p && *p != '/') {
      if (len >= PATH_MAX_NAME - 1) return FS_E_NAMETOOLONG;
      out->parts[out->n][len++] = *p++;
    }
    if (len == 0) {
      if (*p == '/') { p++; continue; }
      return FS_E_INVAL;
    }
    out->parts[out->n][len] = 0;
    if (len == 1 && out->parts[out->n][0] == '.') {
      // "." skip
    } else if (len == 2 && out->parts[out->n][0] == '.' && out->parts[out->n][1] == '.') {
      if (out->n > 0) out->n--;
    } else {
      if (out->n >= PATH_MAX_DEPTH) return FS_E_NAMETOOLONG;
      out->n++;
    }
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
  if (p.n == 0) return true;
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
    char parent_norm[256];
    u64 parent_hash = 0;
    if (dirname_of(norm, parent_norm, sizeof(parent_norm)) > 0)
        parent_hash = hash_name(parent_norm);
    char bn[PATH_MAX_NAME];
    u64 b_val = 0;
    if (basename_of(norm, bn, sizeof(bn)) > 0) {
        for (int i = 0; i < 7 && bn[i]; i++)
            b_val |= ((u64)(u8)bn[i]) << (i*8);
    }
    idx::put(idx::NS_DIR | h, idx::Type::DIR, parent_hash, b_val);
    return 0;
}

struct _EmptyCtx { u64 ph; bool found; };
static void _empty_visit(idx::Rec* r, void* v) {
    _EmptyCtx* c = (_EmptyCtx*)v;
    if (r->a == c->ph) c->found = true;
}

bool dir_is_empty(const char* path) {
    Path p;
    if (parse_path(path, &p) < 0) return false;
    char norm[256];
    if (build_path(&p, norm, sizeof(norm)) < 0) return false;
    _EmptyCtx c = { hash_name(norm), false };
    idx::scan(idx::Type::DIR, _empty_visit, &c);
    return !c.found;
}

i64 fs_rmdir(const char* path) {
    Path p;
    if (parse_path(path, &p) < 0) return FS_E_INVAL;
    if (p.n == 0) return FS_E_INVAL;
    char norm[256];
    if (build_path(&p, norm, sizeof(norm)) < 0) return FS_E_NAMETOOLONG;
    if (!dir_exists(norm)) return FS_E_NOENT;
    if (!dir_is_empty(norm)) return FS_E_NOTEMPTY;
    u64 h = hash_name(norm);
    idx::remove(idx::NS_DIR | h);
    return 0;
}

i64 fs_resolve(const char* rel, const char* cwd, char* out, u32 out_max) {
  if (!rel || !*rel) return FS_E_INVAL;
  char merged[256];
  if (rel[0] == '/') {
    u32 n = 0; while (rel[n]) n++;
    if (n + 1 > (u32)sizeof(merged)) return FS_E_NAMETOOLONG;
    for (u32 i = 0; i <= n; i++) merged[i] = rel[i];
  } else {
    u32 cn = 0; while (cwd[cn]) cn++;
    u32 rn = 0; while (rel[rn]) rn++;
    if (cn + 1 + rn + 1 > (u32)sizeof(merged)) return FS_E_NAMETOOLONG;
    u32 pos = 0;
    for (u32 i = 0; i < cn; i++) merged[pos++] = cwd[i];
    if (cn > 1) merged[pos++] = '/';
    for (u32 i = 0; i < rn; i++) merged[pos + i] = rel[i];
    merged[pos + rn] = 0;
  }
  Path p;
  if (parse_path(merged, &p) < 0) return FS_E_INVAL;
  return build_path(&p, out, out_max);
}

struct _LsCtx { u32 n; };
static void _ls_visit(idx::Rec* r, void* v) {
    _LsCtx* c = (_LsCtx*)v;
    c->n++;
    char nm[9];
    for (int i = 0; i < 8; i++) {
        char ch = (char)((r->b >> (i*8)) & 0xFF);
        if (!ch) { nm[i] = 0; break; }
        nm[i] = ch;
        if (i == 7) nm[8] = 0;
    }
    hal::pf("  %-10s parent=0x%lx\n", nm, r->a);
}

void fs_lsdirs() {
    _LsCtx c = { 0 };
    hal::ps("dirs:\n");
    idx::scan(idx::Type::DIR, _ls_visit, &c);
    hal::pf("  total: %u\n", c.n);
}



// A7b-3b: ls_dir mit parent-hash-Filter
struct _LsDirCtx { u64 parent_hash; u32 count; };
static void _ls_dir_visit(idx::Rec* r, void* v) {
  _LsDirCtx* c = (_LsDirCtx*)v;
  if (r->a != c->parent_hash) return;
  u64 h = r->key & 0x00FFFFFFFFFFFFFFULL;
  idx::Rec* fr = idx::find(idx::NS_FILE | h);
  if (!fr || fr->type != (u32)idx::Type::FILE) return;
  idx::Rec* nr = idx::find(idx::NS_NAME | h);
  char nm[20]; int nn = 0;
  if (nr) {
    for (int i = 0; i < 8; i++) {
      char ch = (char)((nr->a >> (i*8)) & 0xFF);
      if (!ch) break;
      nm[nn++] = ch;
    }
  }
  nm[nn] = 0;
  c->count++;
  hal::ps("  ");
  if (nn > 0) { hal::ps(nm); int pad = 16 - nn; while (pad-- > 0) hal::pc(' '); }
  else        { hal::ps("?               "); }
  hal::pf("size=%u  ref=", (u32)fr->a);
  if (fr->a <= 8) hal::ps("(inline)");
  else if (fr->a <= 512) hal::pf("sect@%p", fr->b);
  else hal::pf("chain@%p", fr->b);
  hal::pc('\n');
}

void fs_ls_dir(const char* path) {
  char abs[256];
  const char* cwd = task::current ? task::current->cwd : "/";
  if (fs_resolve(path, cwd, abs, sizeof(abs)) < 0) {
    hal::pf("ls: resolve error\n"); return;
  }
  if (!dir_exists(abs)) { hal::pf("ls: %s: not found\n", abs); return; }
  _LsDirCtx c = { hash_name(abs), 0 };
  hal::pf("files in %s:\n", abs);
  idx::scan(idx::Type::PATH, _ls_dir_visit, &c);
  hal::pf("  total: %u\n", c.count);
}

} // namespace fs (A7b-1)
