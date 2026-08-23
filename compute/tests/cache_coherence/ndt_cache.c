#include <linux/fs.h>
#include <linux/file.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/pagemap.h>
#include <linux/uaccess.h>

#include "ndt_cache_ioctl.h"

static int ndt_get_range(unsigned long arg, struct ndt_cache_range *range,
                         struct fd *target, pgoff_t *first, pgoff_t *last)
{
    u64 end;

    if (copy_from_user(range, (void __user *)arg, sizeof(*range)))
        return -EFAULT;
    if (range->target_fd < 0 || range->length == 0)
        return -EINVAL;
    if (range->flags & ~NDT_CACHE_F_UNMAP_MMAP)
        return -EINVAL;
    if (range->offset > U64_MAX - (range->length - 1))
        return -EOVERFLOW;

    end = range->offset + range->length - 1;
    if (end > MAX_LFS_FILESIZE)
        return -EFBIG;

    *target = fdget(range->target_fd);
    if (!target->file)
        return -EBADF;
    if (!S_ISREG(file_inode(target->file)->i_mode)) {
        fdput(*target);
        target->file = NULL;
        return -EINVAL;
    }

    *first = range->offset >> PAGE_SHIFT;
    *last = end >> PAGE_SHIFT;
    return 0;
}

static int ndt_invalidate_range(struct file *file,
                                const struct ndt_cache_range *range,
                                pgoff_t first, pgoff_t last)
{
    struct address_space *mapping = file->f_mapping;
    int ret;

    if (range->flags & NDT_CACHE_F_UNMAP_MMAP)
        unmap_mapping_range(mapping, range->offset, range->length, 1);

    filemap_invalidate_lock(mapping);
    ret = invalidate_inode_pages2_range(mapping, first, last);
    filemap_invalidate_unlock(mapping);
    return ret;
}

static long ndt_cache_ioctl(struct file *control, unsigned int cmd,
                            unsigned long arg)
{
    struct ndt_cache_range range;
    struct fd target = { 0 };
    pgoff_t first, last;
    loff_t end;
    int ret;

    (void)control;
    if (_IOC_TYPE(cmd) != NDT_CACHE_IOC_MAGIC ||
        _IOC_SIZE(cmd) != sizeof(struct ndt_cache_range))
        return -ENOTTY;
    if (cmd != NDT_CACHE_BEGIN_RANGE && cmd != NDT_CACHE_COMPLETE_RANGE)
        return -ENOTTY;

    ret = ndt_get_range(arg, &range, &target, &first, &last);
    if (ret)
        return ret;

    end = range.offset + range.length - 1;
    switch (cmd) {
    case NDT_CACHE_BEGIN_RANGE:
        /* Dirty host pages must reach storage before the LBA route is lent. */
        ret = filemap_write_and_wait_range(target.file->f_mapping,
                                           range.offset, end);
        if (!ret)
            ret = ndt_invalidate_range(target.file, &range, first, last);
        break;
    case NDT_CACHE_COMPLETE_RANGE:
        /* Do not write back here: dirty pages after BEGIN violate the lease. */
        ret = ndt_invalidate_range(target.file, &range, first, last);
        break;
    default:
        ret = -ENOTTY;
        break;
    }

    fdput(target);
    return ret;
}

static const struct file_operations ndt_cache_fops = {
    .owner = THIS_MODULE,
    .unlocked_ioctl = ndt_cache_ioctl,
#ifdef CONFIG_COMPAT
    .compat_ioctl = ndt_cache_ioctl,
#endif
};

static struct miscdevice ndt_cache_device = {
    .minor = MISC_DYNAMIC_MINOR,
    .name = "ndt_cache",
    .fops = &ndt_cache_fops,
    .mode = 0600,
};

static int __init ndt_cache_init(void)
{
    return misc_register(&ndt_cache_device);
}

static void __exit ndt_cache_exit(void)
{
    misc_deregister(&ndt_cache_device);
}

module_init(ndt_cache_init);
module_exit(ndt_cache_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("NDT-BPE");
MODULE_DESCRIPTION("Exact inode page-cache range invalidation for NDT write-back");
