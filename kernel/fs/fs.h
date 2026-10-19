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

} // namespace fs

#endif
