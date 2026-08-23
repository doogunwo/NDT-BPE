#ifndef NDT_CACHE_IOCTL_H
#define NDT_CACHE_IOCTL_H

#include <linux/ioctl.h>
#include <linux/types.h>

#define NDT_CACHE_IOC_MAGIC 0xB7

struct ndt_cache_range {
    __s32 target_fd;
    __u32 flags;
    __u64 offset;
    __u64 length;
};

#define NDT_CACHE_F_UNMAP_MMAP (1U << 0)

#define NDT_CACHE_BEGIN_RANGE \
    _IOW(NDT_CACHE_IOC_MAGIC, 0x01, struct ndt_cache_range)
#define NDT_CACHE_COMPLETE_RANGE \
    _IOW(NDT_CACHE_IOC_MAGIC, 0x02, struct ndt_cache_range)

#endif
