/*
 * Test-only helper that inserts one checksummed entry into a known orphan-file
 * block. The image, physical block, and orphan-inode generation are explicit
 * arguments so this cannot accidentally target a mounted device.
 */

#include <ext4_crc32.h>
#include <ext4_misc.h>
#include <ext4_super.h>
#include <ext4_types.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define EXT4_ORPHAN_BLOCK_MAGIC 0x0B10CA04U

struct orphan_tail {
    uint32_t magic;
    uint32_t checksum;
};

int main(int argc, char **argv)
{
    if (argc != 4) {
        fprintf(stderr, "usage: %s IMAGE PHYSICAL_BLOCK INODE_GENERATION\n",
                argv[0]);
        return 2;
    }

    uint64_t physical = strtoull(argv[2], NULL, 0);
    uint32_t generation = (uint32_t)strtoul(argv[3], NULL, 0);
    FILE *image = fopen(argv[1], "r+b");
    if (!image) {
        perror("fopen");
        return 3;
    }

    struct ext4_sblock sb;
    if (fseek(image, EXT4_SUPERBLOCK_OFFSET, SEEK_SET) != 0 ||
        fread(&sb, 1, sizeof(sb), image) != sizeof(sb)) {
        fprintf(stderr, "failed to read superblock\n");
        fclose(image);
        return 4;
    }

    uint32_t block_size = ext4_sb_get_block_size(&sb);
    uint32_t orphan_inum = ext4_get32(&sb, orphan_file_inum);
    if (ext4_get16(&sb, magic) != EXT4_SUPERBLOCK_MAGIC ||
        !block_size || !orphan_inum) {
        fprintf(stderr, "invalid ext4 orphan-file superblock\n");
        fclose(image);
        return 5;
    }

    uint8_t *block = malloc(block_size);
    if (!block) {
        fclose(image);
        return 6;
    }

    uint64_t offset = physical * block_size;
    if (fseek(image, (long)offset, SEEK_SET) != 0 ||
        fread(block, 1, block_size, image) != block_size) {
        fprintf(stderr, "failed to read orphan block\n");
        free(block);
        fclose(image);
        return 7;
    }

    struct orphan_tail *tail =
        (struct orphan_tail *)(block + block_size - sizeof(*tail));
    if (tail->magic != to_le32(EXT4_ORPHAN_BLOCK_MAGIC)) {
        fprintf(stderr, "orphan block magic mismatch\n");
        free(block);
        fclose(image);
        return 8;
    }

    ((uint32_t *)block)[0] = to_le32(orphan_inum + 1);

    uint32_t le_inum = to_le32(orphan_inum);
    uint32_t le_generation = to_le32(generation);
    uint64_t le_physical = to_le64(physical);
    uint32_t checksum = ext4_sb_get_csum_seed(&sb);
    checksum = ext4_crc32c(checksum, &le_inum, sizeof(le_inum));
    checksum = ext4_crc32c(checksum, &le_generation,
                           sizeof(le_generation));
    checksum = ext4_crc32c(checksum, &le_physical, sizeof(le_physical));
    checksum = ext4_crc32c(checksum, block, block_size - sizeof(*tail));
    tail->checksum = to_le32(checksum);

    if (fseek(image, (long)offset, SEEK_SET) != 0 ||
        fwrite(block, 1, block_size, image) != block_size ||
        fflush(image) != 0) {
        fprintf(stderr, "failed to write orphan block\n");
        free(block);
        fclose(image);
        return 9;
    }

    free(block);
    if (fclose(image) != 0)
        return 10;
    puts("inserted one valid checksummed orphan-file entry");
    return 0;
}
