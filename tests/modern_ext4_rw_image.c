/*
 * Standalone integration test for metadata_csum_seed filesystems.
 * It mounts an image through lwext4, writes a file and a large external
 * xattr, reads the xattr back, and cleanly unmounts for an e2fsck check.
 */

#include <ext4.h>
#include <ext4_errno.h>

#include "file_dev.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int check(int rc, const char *operation)
{
    if (rc == EOK)
        return 0;
    fprintf(stderr, "%s failed: rc=%d\n", operation, rc);
    return 1;
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s IMAGE\n", argv[0]);
        return 2;
    }

    file_dev_name_set(argv[1]);
    struct ext4_blockdev *device = file_dev_get();
    if (!device)
        return 3;

    if (check(ext4_device_register(device, "modern_ext4"),
              "ext4_device_register"))
        return 4;
    if (check(ext4_mount("modern_ext4", "/test/", false), "ext4_mount"))
        return 5;

    int rc = ext4_recover("/test/");
    if (rc != EOK && rc != ENOTSUP) {
        check(rc, "ext4_recover");
        return 6;
    }
    if (check(ext4_journal_start("/test/"), "ext4_journal_start"))
        return 7;
    if (check(ext4_cache_write_back("/test/", true),
              "ext4_cache_write_back"))
        return 8;

    uint8_t data[1024 * 1024];
    for (size_t i = 0; i < sizeof(data); ++i)
        data[i] = (uint8_t)(i * 37u + 11u);

    ext4_file file = {};
    if (check(ext4_fopen(&file, "/test/ext4windows-rw-test.bin", "wb"),
              "ext4_fopen"))
        return 9;

    size_t written = 0;
    rc = ext4_fwrite(&file, data, sizeof(data), &written);
    if (check(rc, "ext4_fwrite") || written != sizeof(data))
        return 10;
    if (check(ext4_fclose(&file), "ext4_fclose"))
        return 11;

    const char xattr_name[] = "user.ext4windows.checksum-seed-test";
    uint8_t xattr_value[3000];
    for (size_t i = 0; i < sizeof(xattr_value); ++i)
        xattr_value[i] = (uint8_t)(i * 13u + 7u);

    if (check(ext4_setxattr("/test/ext4windows-rw-test.bin",
                            xattr_name, strlen(xattr_name),
                            xattr_value, sizeof(xattr_value)),
              "ext4_setxattr"))
        return 12;

    uint8_t xattr_readback[sizeof(xattr_value)] = {};
    size_t xattr_size = 0;
    if (check(ext4_getxattr("/test/ext4windows-rw-test.bin",
                            xattr_name, strlen(xattr_name),
                            xattr_readback, sizeof(xattr_readback),
                            &xattr_size),
              "ext4_getxattr"))
        return 13;
    if (xattr_size != sizeof(xattr_value) ||
        memcmp(xattr_value, xattr_readback, sizeof(xattr_value)) != 0) {
        fprintf(stderr, "xattr readback mismatch\n");
        return 14;
    }

    if (check(ext4_frename("/test/ext4windows-rw-test.bin",
                           "/test/ext4windows-rw-test-renamed.bin"),
              "ext4_frename"))
        return 15;

    ext4_file renamed = {};
    if (check(ext4_fopen(&renamed,
                         "/test/ext4windows-rw-test-renamed.bin", "rb"),
              "ext4_fopen renamed"))
        return 16;

    uint8_t readback[sizeof(data)] = {};
    size_t bytes_read = 0;
    rc = ext4_fread(&renamed, readback, sizeof(readback), &bytes_read);
    if (check(rc, "ext4_fread renamed") || bytes_read != sizeof(readback) ||
        memcmp(data, readback, sizeof(data)) != 0) {
        fprintf(stderr, "renamed file readback mismatch\n");
        return 17;
    }
    if (check(ext4_fclose(&renamed), "ext4_fclose renamed"))
        return 18;

    if (check(ext4_fremove("/test/ext4windows-rw-test-renamed.bin"),
              "ext4_fremove"))
        return 19;

    if (check(ext4_dir_mk("/test/ext4windows-rw-test-dir"),
              "ext4_dir_mk"))
        return 20;

    const uint32_t test_time = 1788293945u;
    if (check(ext4_atime_set("/test/ext4windows-rw-test-dir", test_time),
              "ext4_atime_set") ||
        check(ext4_mtime_set("/test/ext4windows-rw-test-dir", test_time),
              "ext4_mtime_set") ||
        check(ext4_ctime_set("/test/ext4windows-rw-test-dir", test_time),
              "ext4_ctime_set") ||
        check(ext4_crtime_set("/test/ext4windows-rw-test-dir", test_time),
              "ext4_crtime_set"))
        return 21;

    uint32_t atime = 0, mtime = 0, ctime = 0, crtime = 0;
    if (check(ext4_atime_get("/test/ext4windows-rw-test-dir", &atime),
              "ext4_atime_get") ||
        check(ext4_mtime_get("/test/ext4windows-rw-test-dir", &mtime),
              "ext4_mtime_get") ||
        check(ext4_ctime_get("/test/ext4windows-rw-test-dir", &ctime),
              "ext4_ctime_get") ||
        check(ext4_crtime_get("/test/ext4windows-rw-test-dir", &crtime),
              "ext4_crtime_get"))
        return 22;
    if (atime != test_time || mtime != test_time || ctime != test_time ||
        crtime != test_time) {
        fprintf(stderr, "timestamp readback mismatch: %u/%u/%u/%u\n",
                atime, mtime, ctime, crtime);
        return 23;
    }

    if (check(ext4_dir_rm("/test/ext4windows-rw-test-dir"),
              "ext4_dir_rm"))
        return 24;

    /*
     * Windows copy first grows the destination to its final size, then may
     * issue writes out of order. Exercise that exact sequence, including
     * reading an unwritten sparse hole as zero.
     */
    const uint64_t sparse_size = 64ULL * 1024ULL * 1024ULL;
    ext4_file sparse = {};
    if (check(ext4_fopen(&sparse,
                         "/test/ext4windows-sparse-grow-test.bin", "wb"),
              "ext4_fopen sparse"))
        return 25;
    if (check(ext4_ftruncate(&sparse, sparse_size),
              "ext4_ftruncate sparse grow") ||
        ext4_fsize(&sparse) != sparse_size)
        return 26;
    if (check(ext4_fclose(&sparse), "ext4_fclose sparse after grow"))
        return 27;
    memset(&sparse, 0, sizeof(sparse));
    if (check(ext4_fopen(&sparse,
                         "/test/ext4windows-sparse-grow-test.bin", "r+b"),
              "ext4_fopen sparse read-write"))
        return 28;

    uint8_t hole[4096];
    memset(hole, 0xA5, sizeof(hole));
    if (check(ext4_fseek(&sparse, 32ULL * 1024ULL * 1024ULL, SEEK_SET),
              "ext4_fseek sparse hole"))
        return 29;
    size_t hole_read = 0;
    if (check(ext4_fread(&sparse, hole, sizeof(hole), &hole_read),
              "ext4_fread sparse hole") ||
        hole_read != sizeof(hole))
        return 30;
    for (size_t i = 0; i < sizeof(hole); ++i) {
        if (hole[i] != 0) {
            fprintf(stderr, "sparse hole did not read as zero at %zu\n", i);
            return 31;
        }
    }

    for (uint32_t pass = 0; pass < 64; ++pass) {
        uint32_t chunk = pass == 0 ? 2 : (pass == 1 ? 0 :
                         (pass == 2 ? 1 : pass));
        uint64_t offset = (uint64_t)chunk * sizeof(data);
        if (check(ext4_fseek(&sparse, offset, SEEK_SET),
                  "ext4_fseek out-of-order write"))
            return 32;
        written = 0;
        if (check(ext4_fwrite(&sparse, data, sizeof(data), &written),
                  "ext4_fwrite out-of-order") || written != sizeof(data))
            return 33;
    }

    if (check(ext4_fseek(&sparse, 0, SEEK_SET),
              "ext4_fseek full readback"))
        return 34;
    for (uint32_t chunk = 0; chunk < 64; ++chunk) {
        memset(readback, 0, sizeof(readback));
        bytes_read = 0;
        if (check(ext4_fread(&sparse, readback, sizeof(readback), &bytes_read),
                  "ext4_fread full readback") ||
            bytes_read != sizeof(readback) ||
            memcmp(data, readback, sizeof(data)) != 0) {
            fprintf(stderr, "out-of-order write readback mismatch at chunk %u\n",
                    chunk);
            return 35;
        }
    }
    if (check(ext4_fclose(&sparse), "ext4_fclose sparse"))
        return 36;
    if (check(ext4_fremove("/test/ext4windows-sparse-grow-test.bin"),
              "ext4_fremove sparse"))
        return 37;

    if (check(ext4_cache_flush("/test/"), "ext4_cache_flush"))
        return 38;
    if (check(ext4_cache_write_back("/test/", false),
              "ext4_cache_write_back off"))
        return 39;
    if (check(ext4_journal_stop("/test/"), "ext4_journal_stop"))
        return 40;
    if (check(ext4_umount("/test/"), "ext4_umount"))
        return 41;
    if (check(ext4_device_unregister("modern_ext4"),
              "ext4_device_unregister"))
        return 42;

    puts("modern ext4 read/write/xattr/rename/delete/timestamp/sparse test passed");
    return 0;
}
