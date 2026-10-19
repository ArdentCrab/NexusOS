// kernel/dev/virtio_blk.h
#ifndef KERNEL_DEV_VIRTIO_BLK_H
#define KERNEL_DEV_VIRTIO_BLK_H
#include "kernel/hal/hal.h"
namespace virtio_blk {
int init(); u64 capacity(); bool is_ready();
int read_sector(u64 sector, void* buf);
int write_sector(u64 sector, const void* buf);
}
#endif
