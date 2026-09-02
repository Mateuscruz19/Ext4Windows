/* Standalone test for Ext4Windows' read-only orphan-file safety probe. */

#include <ext4.h>
#include <ext4_errno.h>
#include <ext4_misc.h>
#include <ext4_super.h>

#include "file_dev.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
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
    if (argc != 3 ||
        (strcmp(argv[2], "inspect-empty") != 0 &&
         strcmp(argv[2], "clear-if-empty") != 0 &&
         strcmp(argv[2], "expect-refusal") != 0)) {
        fprintf(stderr,
                "usage: %s IMAGE inspect-empty|clear-if-empty|expect-refusal\n",
                argv[0]);
        return 2;
    }

    file_dev_name_set(argv[1]);
    struct ext4_blockdev *device = file_dev_get();
    if (!device)
        return 3;

    if (check(ext4_device_register(device, "orphan_probe"),
              "ext4_device_register"))
        return 4;
    if (check(ext4_mount("orphan_probe", "/probe/", true), "ext4_mount"))
        return 5;

    bool empty = false;
    int probe_rc = ext4_orphan_file_check_empty("/probe/", &empty);
    printf("orphan probe: rc=%d empty=%d\n", probe_rc, empty ? 1 : 0);

    int result = 0;
    if (strcmp(argv[2], "expect-refusal") == 0) {
        if (probe_rc == EOK && empty) {
            fprintf(stderr, "expected refusal but orphan file was accepted\n");
            result = 6;
        }
    } else if (probe_rc != EOK || !empty) {
        fprintf(stderr, "expected a valid empty orphan file\n");
        result = 7;
    } else if (strcmp(argv[2], "clear-if-empty") == 0) {
        struct ext4_sblock *sb = NULL;
        if (check(ext4_get_sblock("/probe/", &sb), "ext4_get_sblock")) {
            result = 8;
        } else {
            uint32_t ro = ext4_get32(sb, features_read_only);
            uint32_t last_orphan = ext4_get32(sb, last_orphan);
            uint16_t state = ext4_get16(sb, state);

            if (!(ro & EXT4_FRO_COM_ORPHAN_PRESENT) || last_orphan != 0 ||
                (state & EXT4_SUPERBLOCK_STATE_ORPHAN_FS)) {
                fprintf(stderr,
                        "unsafe or missing clear precondition: ro=0x%08x "
                        "last_orphan=%u state=0x%04x\n",
                        ro, last_orphan, state);
                result = 9;
            } else {
                ext4_set32(sb, features_read_only,
                           ro & ~EXT4_FRO_COM_ORPHAN_PRESENT);
                if (check(ext4_sb_write(device, sb),
                          "ext4_sb_write clear ORPHAN_PRESENT"))
                    result = 10;
            }
        }
    }

    if (check(ext4_umount("/probe/"), "ext4_umount") && result == 0)
        result = 11;
    if (check(ext4_device_unregister("orphan_probe"),
              "ext4_device_unregister") && result == 0)
        result = 12;

    if (result == 0)
        puts("orphan-file probe test passed");
    return result;
}
