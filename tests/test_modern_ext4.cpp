// Tests for modern ext4 superblock layout and checksum seed handling.

#include <catch_amalgamated.hpp>

#include <cstddef>
#include <cstdint>
#include <cstring>

extern "C" {
#include <ext4_crc32.h>
#include <ext4_super.h>
}

static_assert(sizeof(ext4_sblock) == EXT4_SUPERBLOCK_SIZE);
static_assert(offsetof(ext4_sblock, checksum_seed) == 0x270);
static_assert(offsetof(ext4_sblock, orphan_file_inum) == 0x280);
static_assert(offsetof(ext4_sblock, checksum) == 0x3FC);

TEST_CASE("metadata_csum_seed uses the stored superblock seed",
          "[ext4][checksum]")
{
    ext4_sblock sb = {};
    ext4_set32(&sb, features_incompatible, EXT4_FINCOM_CSUM_SEED);
    ext4_set32(&sb, checksum_seed, 0x89ABCDEFu);

    CHECK(ext4_sb_get_csum_seed(&sb) == 0x89ABCDEFu);
}

TEST_CASE("legacy metadata_csum derives the seed from the UUID",
          "[ext4][checksum]")
{
    ext4_sblock sb = {};
    for (uint8_t i = 0; i < sizeof(sb.uuid); ++i)
        sb.uuid[i] = i;

    uint32_t expected = ext4_crc32c(EXT4_CRC32_INIT, sb.uuid,
                                    sizeof(sb.uuid));
    CHECK(ext4_sb_get_csum_seed(&sb) == expected);
}
