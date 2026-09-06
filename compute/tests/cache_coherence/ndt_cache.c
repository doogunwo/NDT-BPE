#include <linux/fs.h>
#include <linux/file.h>
#include <linux/list.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pagemap.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/uaccess.h>

#include "ndt_cache_ioctl.h"

struct ndt_cache_control {
    struct mutex lock;
    struct list_head lease_node;
    struct file *target;
    struct ndt_cache_range range;
    pgoff_t first;
    pgoff_t last;
    bool write_denied;
    bool registered;
};

static LIST_HEAD(ndt_active_leases);
static DEFINE_MUTEX(ndt_active_leases_lock);

static int ndt_register_lease(struct ndt_cache_control *state)
{
    struct ndt_cache_control *other;
    struct inode *inode = file_inode(state->target);
    int ret = 0;

    mutex_lock(&ndt_active_leases_lock);
    list_for_each_entry(other, &ndt_active_leases, lease_node) {
        if (file_inode(other->target) == inode) {
            ret = -EBUSY;
            goto out;
        }
    }
    list_add(&state->lease_node, &ndt_active_leases);
    state->registered = true;
out:
    mutex_unlock(&ndt_active_leases_lock);
    return ret;
}

static void ndt_unregister_lease(struct ndt_cache_control *state)
{
    if (!state->registered)
        return;
    mutex_lock(&ndt_active_leases_lock);
    list_del_init(&state->lease_node);
    state->registered = false;
    mutex_unlock(&ndt_active_leases_lock);
}

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

static void ndt_release_lease_locked(struct ndt_cache_control *state,
                                     bool invalidate)
{
    if (!state->target)
        return;

    if (invalidate)
        ndt_invalidate_range(state->target, &state->range,
                             state->first, state->last);
    if (state->write_denied)
        allow_write_access(state->target);
    ndt_unregister_lease(state);
    fput(state->target);
    state->target = NULL;
    state->write_denied = false;
    memset(&state->range, 0, sizeof(state->range));
}

static int ndt_cache_open(struct inode *inode, struct file *control)
{
    struct ndt_cache_control *state;

    (void)inode;
    state = kzalloc(sizeof(*state), GFP_KERNEL);
    if (!state)
        return -ENOMEM;
    mutex_init(&state->lock);
    INIT_LIST_HEAD(&state->lease_node);
    control->private_data = state;
    return 0;
}

static int ndt_cache_release(struct inode *inode, struct file *control)
{
    struct ndt_cache_control *state = control->private_data;

    (void)inode;
    if (!state)
        return 0;
    mutex_lock(&state->lock);
    /* A crashed owner must not leave the inode permanently write-denied. */
    ndt_release_lease_locked(state, true);
    mutex_unlock(&state->lock);
    kfree(state);
    control->private_data = NULL;
    return 0;
}

static long ndt_cache_ioctl(struct file *control, unsigned int cmd,
                            unsigned long arg)
{
    struct ndt_cache_range range;
    struct ndt_cache_control *state = control->private_data;
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

    if (!state)
        return -EBADF;
    ret = ndt_get_range(arg, &range, &target, &first, &last);
    if (ret)
        return ret;

    end = range.offset + range.length - 1;
    mutex_lock(&state->lock);
    switch (cmd) {
    case NDT_CACHE_BEGIN_RANGE:
        if (state->target) {
            ret = -EBUSY;
            break;
        }
        /*
         * The caller passes a read-only FD.  deny_write_access() fails when
         * another writable FD already exists and prevents new writable opens
         * until COMPLETE or control-FD release.
         */
        get_file(target.file);
        state->target = target.file;
        state->range = range;
        state->first = first;
        state->last = last;
        ret = ndt_register_lease(state);
        if (ret) {
            ndt_release_lease_locked(state, false);
            break;
        }
        ret = deny_write_access(state->target);
        if (ret)
            goto begin_fail;
        state->write_denied = true;
        if (mapping_writably_mapped(state->target->f_mapping)) {
            ret = -EBUSY;
            goto begin_fail;
        }
        /* Dirty host pages must reach storage before the LBA route is lent. */
        ret = filemap_write_and_wait_range(state->target->f_mapping,
                                           range.offset, end);
        if (!ret)
            ret = ndt_invalidate_range(state->target, &range, first, last);
        if (ret)
            goto begin_fail;
        break;
begin_fail:
        ndt_release_lease_locked(state, false);
        break;
    case NDT_CACHE_COMPLETE_RANGE:
        if (!state->target) {
            ret = -ENOENT;
            break;
        }
        if (state->target->f_mapping != target.file->f_mapping ||
            state->range.offset != range.offset ||
            state->range.length != range.length) {
            ret = -EINVAL;
            break;
        }
        /* Do not write back here: dirty pages after BEGIN violate the lease. */
        ret = ndt_invalidate_range(state->target, &state->range,
                                   state->first, state->last);
        if (!ret)
            ndt_release_lease_locked(state, false);
        break;
    default:
        ret = -ENOTTY;
        break;
    }

    mutex_unlock(&state->lock);
    fdput(target);
    return ret;
}

static const struct file_operations ndt_cache_fops = {
    .owner = THIS_MODULE,
    .open = ndt_cache_open,
    .release = ndt_cache_release,
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
