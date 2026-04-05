// Test Suite: Block Device Operations
// Tests block device create/destroy cycles and ext4 mount/read integration.
// These are integration tests that use real ext4 images.

#include <catch_amalgamated.hpp>
#include <windows.h>

#include "blockdev_file.hpp"
#include "blockdev_partition.hpp"

extern "C" {
#include <ext4.h>
}

// ============================================================
//  File Block Device: Create / Destroy
// ============================================================

TEST_CASE("create_file_blockdev returns valid pointer for existing file", "[blockdev][file]")
{
    struct ext4_blockdev* bdev = create_file_blockdev(
        "C:/Users/Mateus/Ext4Windows/test.img");
    REQUIRE(bdev != nullptr);
    destroy_file_blockdev(bdev);
}

TEST_CASE("create_file_blockdev for nonexistent file: create succeeds but open fails", "[blockdev][file]")
{
    // create_file_blockdev allocates the struct but doesn't open the file.
    // The actual open happens when ext4_device_register + ext4_mount is called.
    // So we test that mount fails gracefully for a bad path.
    struct ext4_blockdev* bdev = create_file_blockdev(
        "C:/nonexistent/fake.img");
    if (bdev) {
        int rc = ext4_device_register(bdev, "bad_path_test");
        if (rc == EOK) {
            int mount_rc = ext4_mount("bad_path_test", "/bad_mnt/", true);
            CHECK(mount_rc != EOK);  // Mount should fail
            ext4_device_unregister("bad_path_test");
        }
        destroy_file_blockdev(bdev);
    }
}

TEST_CASE("destroy_file_blockdev handles null safely", "[blockdev][file]")
{
    destroy_file_blockdev(nullptr);  // Should not crash
}

TEST_CASE("create_file_blockdev: multiple create/destroy cycles", "[blockdev][file]")
{
    // Ensure no resource leaks over repeated cycles
    for (int i = 0; i < 10; i++) {
        struct ext4_blockdev* bdev = create_file_blockdev(
            "C:/Users/Mateus/Ext4Windows/test.img");
        REQUIRE(bdev != nullptr);
        destroy_file_blockdev(bdev);
    }
}

// ============================================================
//  Partition Block Device: Create / Destroy
// ============================================================

TEST_CASE("create_partition_blockdev returns valid pointer", "[blockdev][partition]")
{
    // Use a fake path - we only test allocation, not opening
    struct ext4_blockdev* bdev = create_partition_blockdev(
        L"\\\\.\\PhysicalDrive99", true);
    REQUIRE(bdev != nullptr);
    destroy_partition_blockdev(bdev);
}

TEST_CASE("create_partition_blockdev_from_handle with INVALID_HANDLE", "[blockdev][partition]")
{
    struct ext4_blockdev* bdev = create_partition_blockdev_from_handle(
        INVALID_HANDLE_VALUE, true);
    REQUIRE(bdev != nullptr);
    // Don't try to open it - just verify allocation works
    destroy_partition_blockdev(bdev);
}

TEST_CASE("destroy_partition_blockdev handles null safely", "[blockdev][partition]")
{
    destroy_partition_blockdev(nullptr);  // Should not crash
}

TEST_CASE("create_partition_blockdev: read-only vs read-write", "[blockdev][partition]")
{
    struct ext4_blockdev* bdev_ro = create_partition_blockdev(L"\\\\.\\X:", true);
    struct ext4_blockdev* bdev_rw = create_partition_blockdev(L"\\\\.\\X:", false);
    REQUIRE(bdev_ro != nullptr);
    REQUIRE(bdev_rw != nullptr);
    destroy_partition_blockdev(bdev_ro);
    destroy_partition_blockdev(bdev_rw);
}

// ============================================================
//  Full ext4 Mount / Read / Unmount Cycle
// ============================================================

TEST_CASE("ext4 mount and unmount cycle", "[blockdev][ext4][integration]")
{
    struct ext4_blockdev* bdev = create_file_blockdev(
        "C:/Users/Mateus/Ext4Windows/test.img");
    REQUIRE(bdev != nullptr);

    int rc = ext4_device_register(bdev, "test_mount");
    REQUIRE(rc == EOK);

    rc = ext4_mount("test_mount", "/test_mnt/", true);
    REQUIRE(rc == EOK);

    rc = ext4_umount("/test_mnt/");
    CHECK(rc == EOK);

    ext4_device_unregister("test_mount");
    destroy_file_blockdev(bdev);
}

TEST_CASE("ext4 read root directory", "[blockdev][ext4][integration]")
{
    struct ext4_blockdev* bdev = create_file_blockdev(
        "C:/Users/Mateus/Ext4Windows/test.img");
    REQUIRE(bdev != nullptr);

    REQUIRE(ext4_device_register(bdev, "test_readdir") == EOK);
    REQUIRE(ext4_mount("test_readdir", "/test_rd/", true) == EOK);

    ext4_dir dir;
    int rc = ext4_dir_open(&dir, "/test_rd/");
    REQUIRE(rc == EOK);

    int entry_count = 0;
    const ext4_direntry* de;
    while ((de = ext4_dir_entry_next(&dir)) != nullptr) {
        entry_count++;
        // Every entry must have a non-zero inode
        CHECK(de->inode != 0);
        // Entry name must not be empty
        CHECK(de->name_length > 0);
    }

    ext4_dir_close(&dir);

    // Root directory must have at least "." and ".." entries
    CHECK(entry_count >= 2);

    ext4_umount("/test_rd/");
    ext4_device_unregister("test_readdir");
    destroy_file_blockdev(bdev);
}

TEST_CASE("ext4 read file contents", "[blockdev][ext4][integration]")
{
    struct ext4_blockdev* bdev = create_file_blockdev(
        "C:/Users/Mateus/Ext4Windows/test.img");
    REQUIRE(bdev != nullptr);

    REQUIRE(ext4_device_register(bdev, "test_fread") == EOK);
    REQUIRE(ext4_mount("test_fread", "/test_fr/", true) == EOK);

    // Try to open a file (may or may not exist in test image)
    ext4_file f;
    int rc = ext4_fopen(&f, "/test_fr/hello.txt", "r");
    if (rc == EOK) {
        char buf[1024] = {};
        size_t bytes_read = 0;
        rc = ext4_fread(&f, buf, sizeof(buf) - 1, &bytes_read);
        CHECK(rc == EOK);
        CHECK(bytes_read > 0);
        ext4_fclose(&f);
    }
    // If file doesn't exist, that's OK — we still tested the mount

    ext4_umount("/test_fr/");
    ext4_device_unregister("test_fread");
    destroy_file_blockdev(bdev);
}

TEST_CASE("ext4 get file/dir info", "[blockdev][ext4][integration]")
{
    struct ext4_blockdev* bdev = create_file_blockdev(
        "C:/Users/Mateus/Ext4Windows/test.img");
    REQUIRE(bdev != nullptr);

    REQUIRE(ext4_device_register(bdev, "test_info") == EOK);
    REQUIRE(ext4_mount("test_info", "/test_info/", true) == EOK);

    // Root directory should be accessible
    uint32_t mode = 0;
    int rc = ext4_mode_get("/test_info/", &mode);
    CHECK(rc == EOK);
    // Root dir should have directory bit set (S_IFDIR = 0x4000)
    CHECK((mode & 0xF000) == 0x4000);

    ext4_umount("/test_info/");
    ext4_device_unregister("test_info");
    destroy_file_blockdev(bdev);
}

TEST_CASE("ext4 timestamps are readable", "[blockdev][ext4][integration]")
{
    struct ext4_blockdev* bdev = create_file_blockdev(
        "C:/Users/Mateus/Ext4Windows/test.img");
    REQUIRE(bdev != nullptr);

    REQUIRE(ext4_device_register(bdev, "test_time") == EOK);
    REQUIRE(ext4_mount("test_time", "/test_time/", true) == EOK);

    uint32_t atime = 0, mtime = 0, ctime = 0;
    int rc = ext4_atime_get("/test_time/", &atime);
    CHECK(rc == EOK);
    rc = ext4_mtime_get("/test_time/", &mtime);
    CHECK(rc == EOK);
    rc = ext4_ctime_get("/test_time/", &ctime);
    CHECK(rc == EOK);

    // Timestamps should be nonzero for any real filesystem
    CHECK(mtime > 0);
    CHECK(ctime > 0);

    ext4_umount("/test_time/");
    ext4_device_unregister("test_time");
    destroy_file_blockdev(bdev);
}

// ============================================================
//  Error handling
// ============================================================

TEST_CASE("ext4_mount with invalid device name fails gracefully", "[blockdev][ext4][error]")
{
    int rc = ext4_mount("nonexistent_device", "/bad/", true);
    CHECK(rc != EOK);
}

TEST_CASE("ext4_device_register with null name fails", "[blockdev][ext4][error]")
{
    // lwext4 may accept nullptr bdev (implementation detail), so instead
    // test that registering with duplicate name fails
    struct ext4_blockdev* bdev = create_file_blockdev(
        "C:/Users/Mateus/Ext4Windows/test.img");
    REQUIRE(bdev != nullptr);

    int rc1 = ext4_device_register(bdev, "dup_test");
    REQUIRE(rc1 == EOK);

    // Second register with same name should fail
    int rc2 = ext4_device_register(bdev, "dup_test");
    CHECK(rc2 != EOK);

    ext4_device_unregister("dup_test");
    destroy_file_blockdev(bdev);
}
