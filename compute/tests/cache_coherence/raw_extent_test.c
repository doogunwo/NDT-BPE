#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <linux/fiemap.h>
#include <linux/fs.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "ndt_cache_ioctl.h"

enum { BLOCK_BYTES = 4096 };

static void die(const char *what)
{
    perror(what);
    exit(EXIT_FAILURE);
}

static void require_pattern(const unsigned char *buf, unsigned char pattern,
                            const char *phase)
{
    size_t i;
    for (i = 0; i < BLOCK_BYTES; ++i) {
        if (buf[i] != pattern) {
            fprintf(stderr, "%s: byte %zu is 0x%02x, expected 0x%02x\n",
                    phase, i, buf[i], pattern);
            exit(EXIT_FAILURE);
        }
    }
}

static void raw_write_pattern(const char *block_path, uint64_t physical,
                              unsigned char pattern, unsigned char *direct)
{
    int block_fd;

    memset(direct, pattern, BLOCK_BYTES);
    block_fd = open(block_path, O_RDWR | O_DIRECT | O_CLOEXEC);
    if (block_fd < 0)
        die("open block device");
    if (pwrite(block_fd, direct, BLOCK_BYTES, (off_t)physical) != BLOCK_BYTES)
        die("raw block write");
    if (fsync(block_fd))
        die("fsync block device");
    close(block_fd);
}

int main(int argc, char **argv)
{
    struct {
        struct fiemap map;
        struct fiemap_extent extent;
    } fm;
    struct ndt_cache_range range = { 0 };
    unsigned char cached[BLOCK_BYTES];
    unsigned char *direct = NULL;
    uint64_t physical;
    int file_fd, control_fd, second_control_fd, writer_fd;
    void *writable_map;

    if (argc != 4) {
        fprintf(stderr, "usage: %s TEST_FILE BLOCK_DEVICE CONTROL_DEVICE\n", argv[0]);
        return EXIT_FAILURE;
    }

    file_fd = open(argv[1], O_CREAT | O_TRUNC | O_RDWR | O_CLOEXEC, 0600);
    if (file_fd < 0)
        die("open test file");
    memset(cached, 'A', sizeof(cached));
    if (pwrite(file_fd, cached, sizeof(cached), 0) != sizeof(cached))
        die("write pattern A");
    if (fdatasync(file_fd))
        die("fdatasync pattern A");

    memset(&fm, 0, sizeof(fm));
    fm.map.fm_start = 0;
    fm.map.fm_length = BLOCK_BYTES;
    fm.map.fm_extent_count = 1;
    if (ioctl(file_fd, FS_IOC_FIEMAP, &fm) < 0)
        die("FIEMAP");
    if (fm.map.fm_mapped_extents != 1 || fm.extent.fe_length < BLOCK_BYTES ||
        (fm.extent.fe_flags & (FIEMAP_EXTENT_UNKNOWN | FIEMAP_EXTENT_DELALLOC |
                               FIEMAP_EXTENT_UNWRITTEN))) {
        fprintf(stderr, "test file does not have one stable written extent\n");
        return EXIT_FAILURE;
    }
    physical = fm.extent.fe_physical;
    if (physical % BLOCK_BYTES) {
        fprintf(stderr, "physical extent is not 4-KiB aligned\n");
        return EXIT_FAILURE;
    }

    /* Populate the file page cache with A before the out-of-band write. */
    if (pread(file_fd, cached, sizeof(cached), 0) != sizeof(cached))
        die("buffered read A");
    require_pattern(cached, 'A', "initial buffered read");

    if (posix_memalign((void **)&direct, BLOCK_BYTES, BLOCK_BYTES)) {
        fprintf(stderr, "posix_memalign failed\n");
        return EXIT_FAILURE;
    }
    raw_write_pattern(argv[2], physical, 'B', direct);

    if (pread(file_fd, cached, sizeof(cached), 0) != sizeof(cached))
        die("stale buffered read");
    require_pattern(cached, 'A', "buffered read before invalidation");
    puts("OBSERVED stale buffered page after raw physical-block write");

    close(file_fd);
    file_fd = open(argv[1], O_RDONLY | O_CLOEXEC);
    if (file_fd < 0)
        die("open read-only lease target");
    range.target_fd = file_fd;
    range.offset = 0;
    range.length = BLOCK_BYTES;

    /* An already-open writer must make lease acquisition fail. */
    writer_fd = open(argv[1], O_RDWR | O_CLOEXEC);
    if (writer_fd < 0)
        die("open conflicting writer");
    control_fd = open(argv[3], O_RDONLY | O_CLOEXEC);
    if (control_fd < 0)
        die("open NDT cache control");
    errno = 0;
    if (ioctl(control_fd, NDT_CACHE_BEGIN_RANGE, &range) == 0 || errno != ETXTBSY) {
        fprintf(stderr, "BEGIN accepted an existing writable FD (errno=%d)\n", errno);
        return EXIT_FAILURE;
    }
    close(control_fd);
    close(writer_fd);
    puts("PASS: BEGIN rejected an existing writable FD");

    /* A writable mmap remains dangerous even after its FD is closed. */
    writer_fd = open(argv[1], O_RDWR | O_CLOEXEC);
    if (writer_fd < 0)
        die("open writable mmap file");
    writable_map = mmap(NULL, BLOCK_BYTES, PROT_READ | PROT_WRITE,
                        MAP_SHARED, writer_fd, 0);
    if (writable_map == MAP_FAILED)
        die("create writable mmap");
    close(writer_fd);
    control_fd = open(argv[3], O_RDONLY | O_CLOEXEC);
    if (control_fd < 0)
        die("open control for mmap rejection");
    errno = 0;
    if (ioctl(control_fd, NDT_CACHE_BEGIN_RANGE, &range) == 0 ||
        (errno != EBUSY && errno != ETXTBSY)) {
        fprintf(stderr, "BEGIN accepted a writable mmap (errno=%d)\n", errno);
        return EXIT_FAILURE;
    }
    close(control_fd);
    if (munmap(writable_map, BLOCK_BYTES))
        die("remove writable mmap");
    puts("PASS: BEGIN rejected an existing writable mmap");

    /* Acquire the lease, then prove that new writable opens are denied. */
    control_fd = open(argv[3], O_RDONLY | O_CLOEXEC);
    if (control_fd < 0)
        die("open control for active lease");
    if (ioctl(control_fd, NDT_CACHE_BEGIN_RANGE, &range) < 0)
        die("NDT_CACHE_BEGIN_RANGE");
    second_control_fd = open(argv[3], O_RDONLY | O_CLOEXEC);
    if (second_control_fd < 0)
        die("open second lease control");
    errno = 0;
    if (ioctl(second_control_fd, NDT_CACHE_BEGIN_RANGE, &range) == 0 ||
        errno != EBUSY) {
        fprintf(stderr, "second control acquired the same inode (errno=%d)\n", errno);
        return EXIT_FAILURE;
    }
    close(second_control_fd);
    puts("PASS: global registry rejected a second lease on the same inode");

    errno = 0;
    writer_fd = open(argv[1], O_WRONLY | O_CLOEXEC);
    if (writer_fd >= 0 || errno != ETXTBSY) {
        fprintf(stderr, "active lease allowed a new writer (errno=%d)\n", errno);
        return EXIT_FAILURE;
    }
    puts("PASS: active lease denied a new writable open");
    errno = 0;
    if (truncate(argv[1], 0) == 0 || errno != ETXTBSY) {
        fprintf(stderr, "active lease allowed truncate (errno=%d)\n", errno);
        return EXIT_FAILURE;
    }
    puts("PASS: active lease denied truncate");

    /* BEGIN removed stale A.  Cache C during the lease, overwrite it behind
     * ext4 with D, and verify that COMPLETE removes the newly stale C page. */
    raw_write_pattern(argv[2], physical, 'C', direct);
    if (pread(file_fd, cached, sizeof(cached), 0) != sizeof(cached))
        die("buffered read C during lease");
    require_pattern(cached, 'C', "buffered read during lease");
    raw_write_pattern(argv[2], physical, 'D', direct);

    if (ioctl(control_fd, NDT_CACHE_COMPLETE_RANGE, &range) < 0)
        die("NDT_CACHE_COMPLETE_RANGE");
    close(control_fd);
    if (pread(file_fd, cached, sizeof(cached), 0) != sizeof(cached))
        die("buffered read after BEGIN/COMPLETE");
    require_pattern(cached, 'D', "buffered read after BEGIN/COMPLETE");
    puts("PASS: COMPLETE invalidated cache populated during the lease");

    writer_fd = open(argv[1], O_WRONLY | O_CLOEXEC);
    if (writer_fd < 0)
        die("writer remained denied after COMPLETE");
    memset(cached, 'E', sizeof(cached));
    if (pwrite(writer_fd, cached, sizeof(cached), 0) != sizeof(cached))
        die("dirty buffered write E");
    close(writer_fd);

    /* Dirty E remains eligible for writeback after its writer closes.  BEGIN
     * must write it back and invalidate it before storage writes F. */
    control_fd = open(argv[3], O_RDONLY | O_CLOEXEC);
    if (control_fd < 0)
        die("open control for dirty-page test");
    if (ioctl(control_fd, NDT_CACHE_BEGIN_RANGE, &range) < 0)
        die("BEGIN for dirty-page test");
    raw_write_pattern(argv[2], physical, 'F', direct);
    if (ioctl(control_fd, NDT_CACHE_COMPLETE_RANGE, &range) < 0)
        die("COMPLETE for dirty-page test");
    close(control_fd);
    if (pread(file_fd, cached, sizeof(cached), 0) != sizeof(cached))
        die("read after dirty-page test");
    require_pattern(cached, 'F', "dirty-page test result");
    puts("PASS: BEGIN removed dirty host state before raw pattern F");

    /* Closing a control FD without COMPLETE simulates process cleanup. */
    control_fd = open(argv[3], O_RDONLY | O_CLOEXEC);
    if (control_fd < 0)
        die("open control for release test");
    if (ioctl(control_fd, NDT_CACHE_BEGIN_RANGE, &range) < 0)
        die("BEGIN for release test");
    close(control_fd);
    writer_fd = open(argv[1], O_WRONLY | O_CLOEXEC);
    if (writer_fd < 0)
        die("control-FD release left inode write-denied");
    close(writer_fd);
    puts("PASS: control-FD release automatically released the lease");

    free(direct);
    close(file_fd);
    return EXIT_SUCCESS;
}
