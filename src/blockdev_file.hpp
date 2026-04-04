#pragma once

#include <cstdint>

extern "C" {
#include <ext4_blockdev.h>
}

// Creates an ext4_blockdev backed by a file (e.g., a .img file).
// The caller takes ownership and must call destroy_file_blockdev() when done.
struct ext4_blockdev* create_file_blockdev(const char* path);
void destroy_file_blockdev(struct ext4_blockdev* bdev);
