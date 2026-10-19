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

} // namespace fs

#endif
