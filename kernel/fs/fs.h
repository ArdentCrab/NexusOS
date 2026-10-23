// kernel/fs/fs.h — Refactor Session 5
#ifndef KERNEL_FS_FS_H
#define KERNEL_FS_FS_H

#include "kernel/hal/hal.h"

namespace fs {

// --- Datei-Operationen (Name-basiert, ueber idx-FILE-Records) ---
u64  hash_name(const char* s);
int  cat_file(const char* name, char* out, usize out_max);
int  write_file(const char* name, const char* content, usize len);
void ls_files();
int  rm_file(const char* name);

// --- Bitmap / Block-Allocation ---
void bitmap_load();
void bitmap_save();
u64  blk_alloc_nosave();
void blk_free(u64 sector);
u64  used_data_sectors();
void rebuild_bitmap_from_index();
int  bm_dirty_count();
u64  bm_write_count();

// --- Pfad-Handling (A7a) ---
static constexpr u32 PATH_MAX_DEPTH = 8;
static constexpr u32 PATH_MAX_NAME  = 32;

enum : i64 {
    FS_E_INVAL       = -22,
    FS_E_NAMETOOLONG = -36,
    FS_E_NOTEMPTY    = -39,
    FS_E_NOENT       = -2,
    FS_E_EXIST       = -17,
};

struct Path {
    char parts[PATH_MAX_DEPTH][PATH_MAX_NAME];
    u32  n;
};

// Zerlegt "/a/b/c" in {a, b, c}. Rueckgabe 0 oder -errno.
i64 parse_path(const char* path, Path* out);

// Baut aus Path einen normalisierten Pfad "/a/b/c". Rueckgabe Laenge oder -errno.
i64 build_path(const Path* p, char* out, u32 out_max);

// Kuerzt "/a/b/c" auf "/a/b". Rueckgabe Laenge oder -errno.
i64 dirname_of(const char* path, char* out, u32 out_max);

// Liefert den letzten Komponenten "/a/b/c" -> "c". Rueckgabe Laenge oder -errno.
i64 basename_of(const char* path, char* out, u32 out_max);

i64 fs_mkdir(const char* path);
i64 fs_rmdir(const char* path);
bool dir_exists(const char* path);


// A7b-2: resolve rel gegen cwd. Rueckgabe Laenge oder -errno.
i64  fs_resolve(const char* rel, const char* cwd, char* out, u32 out_max);
bool dir_is_empty(const char* path);
void fs_lsdirs();


// A7b-3a: absolute Pfad-API
i64  fs_write_abs(const char* abs, const char* content, usize len);
i64  fs_cat_abs(const char* abs, char* out, usize out_max);

// A-S7: liest N Bytes ab offset aus einer Datei (per FILE-Record-Key).
// Kein Heap-Wachstum, unabhaengig von der Dateigroesse.
// Rueckgabe: Anzahl gelesener Bytes (>=0) oder -errno.
i64  fs_read_at(u64 file_key, u64 offset, u8* buf, u32 len);
void migrate_files_v2();


// A7b-3b: absolute pfad-based
i64  fs_rm_abs(const char* abs);
void fs_ls_dir(const char* path);

} // namespace fs

#endif
