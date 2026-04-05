// Test Suite: Permission Mapping
// Tests ext4 POSIX mode bits → Windows file attributes conversion.
// Critical for correct file display in Explorer.

#include <catch_amalgamated.hpp>
#include "test_utils.hpp"

using namespace ext4windows;

// ============================================================
//  Regular Files: Owner Write Permission
// ============================================================

TEST_CASE("Regular file with full permissions (0777) is not readonly", "[permissions]")
{
    uint32_t attrs = ext4_mode_to_win_attributes(0777, false, "file.txt");
    CHECK((attrs & FILE_ATTRIBUTE_READONLY) == 0);
    CHECK((attrs & FILE_ATTRIBUTE_NORMAL) != 0);
}

TEST_CASE("Regular file with 0755 is not readonly", "[permissions]")
{
    // rwxr-xr-x — owner has write
    uint32_t attrs = ext4_mode_to_win_attributes(0755, false, "script.sh");
    CHECK((attrs & FILE_ATTRIBUTE_READONLY) == 0);
}

TEST_CASE("Regular file with 0644 is not readonly", "[permissions]")
{
    // rw-r--r-- — owner has write
    uint32_t attrs = ext4_mode_to_win_attributes(0644, false, "document.txt");
    CHECK((attrs & FILE_ATTRIBUTE_READONLY) == 0);
}

TEST_CASE("Regular file with 0444 is readonly", "[permissions]")
{
    // r--r--r-- — nobody has write
    uint32_t attrs = ext4_mode_to_win_attributes(0444, false, "readonly.txt");
    CHECK((attrs & FILE_ATTRIBUTE_READONLY) != 0);
}

TEST_CASE("Regular file with 0555 is readonly", "[permissions]")
{
    // r-xr-xr-x — no write for anyone
    uint32_t attrs = ext4_mode_to_win_attributes(0555, false, "binary");
    CHECK((attrs & FILE_ATTRIBUTE_READONLY) != 0);
}

TEST_CASE("Regular file with 0400 is readonly", "[permissions]")
{
    // r-------- — owner read only
    uint32_t attrs = ext4_mode_to_win_attributes(0400, false, "secret.key");
    CHECK((attrs & FILE_ATTRIBUTE_READONLY) != 0);
}

TEST_CASE("Regular file with 0200 is not readonly", "[permissions]")
{
    // -w------- — owner write only (weird but valid)
    uint32_t attrs = ext4_mode_to_win_attributes(0200, false, "writeonly");
    CHECK((attrs & FILE_ATTRIBUTE_READONLY) == 0);
}

TEST_CASE("Regular file with 0000 is readonly", "[permissions]")
{
    // --------- — no permissions at all
    uint32_t attrs = ext4_mode_to_win_attributes(0000, false, "noperm");
    CHECK((attrs & FILE_ATTRIBUTE_READONLY) != 0);
}

// ============================================================
//  Directories: Never Readonly
// ============================================================

TEST_CASE("Directory with 0555 is NOT readonly", "[permissions][directory]")
{
    // Directories should never get READONLY attribute on Windows
    // (it would prevent Explorer from listing contents)
    uint32_t attrs = ext4_mode_to_win_attributes(0555, true, "mydir");
    CHECK((attrs & FILE_ATTRIBUTE_READONLY) == 0);
    CHECK((attrs & FILE_ATTRIBUTE_DIRECTORY) != 0);
}

TEST_CASE("Directory with 0755 has DIRECTORY attribute", "[permissions][directory]")
{
    uint32_t attrs = ext4_mode_to_win_attributes(0755, true, "home");
    CHECK((attrs & FILE_ATTRIBUTE_DIRECTORY) != 0);
}

TEST_CASE("Directory with 0000 still not readonly", "[permissions][directory]")
{
    uint32_t attrs = ext4_mode_to_win_attributes(0000, true, "locked");
    CHECK((attrs & FILE_ATTRIBUTE_READONLY) == 0);
    CHECK((attrs & FILE_ATTRIBUTE_DIRECTORY) != 0);
}

// ============================================================
//  Hidden Files (dot prefix)
// ============================================================

TEST_CASE("Files starting with dot are hidden", "[permissions][hidden]")
{
    uint32_t attrs = ext4_mode_to_win_attributes(0644, false, ".bashrc");
    CHECK((attrs & FILE_ATTRIBUTE_HIDDEN) != 0);
}

TEST_CASE("Directories starting with dot are hidden", "[permissions][hidden]")
{
    uint32_t attrs = ext4_mode_to_win_attributes(0755, true, ".config");
    CHECK((attrs & FILE_ATTRIBUTE_HIDDEN) != 0);
}

TEST_CASE(".hidden files list", "[permissions][hidden]")
{
    const char* hidden_names[] = {
        ".bashrc", ".profile", ".gitignore", ".ssh", ".config",
        ".local", ".cache", ".vimrc", ".env", ".hidden"
    };
    for (const char* name : hidden_names) {
        INFO("Testing: " << name);
        uint32_t attrs = ext4_mode_to_win_attributes(0644, false, name);
        CHECK((attrs & FILE_ATTRIBUTE_HIDDEN) != 0);
    }
}

TEST_CASE("Files NOT starting with dot are not hidden", "[permissions][hidden]")
{
    uint32_t attrs = ext4_mode_to_win_attributes(0644, false, "readme.txt");
    CHECK((attrs & FILE_ATTRIBUTE_HIDDEN) == 0);
}

TEST_CASE("Null basename doesn't crash", "[permissions][hidden]")
{
    // Should not crash when basename is null
    uint32_t attrs = ext4_mode_to_win_attributes(0644, false, nullptr);
    CHECK((attrs & FILE_ATTRIBUTE_HIDDEN) == 0);
}

// ============================================================
//  NORMAL attribute logic
// ============================================================

TEST_CASE("NORMAL is set for plain regular file", "[permissions][attributes]")
{
    uint32_t attrs = ext4_mode_to_win_attributes(0644, false, "file.txt");
    CHECK((attrs & FILE_ATTRIBUTE_NORMAL) != 0);
}

TEST_CASE("NORMAL is cleared when READONLY is set", "[permissions][attributes]")
{
    // Windows rule: NORMAL and READONLY are mutually exclusive
    uint32_t attrs = ext4_mode_to_win_attributes(0444, false, "readonly.txt");
    CHECK((attrs & FILE_ATTRIBUTE_READONLY) != 0);
    CHECK((attrs & FILE_ATTRIBUTE_NORMAL) == 0);
}

TEST_CASE("NORMAL is cleared when HIDDEN is set", "[permissions][attributes]")
{
    uint32_t attrs = ext4_mode_to_win_attributes(0644, false, ".hidden");
    CHECK((attrs & FILE_ATTRIBUTE_HIDDEN) != 0);
    CHECK((attrs & FILE_ATTRIBUTE_NORMAL) == 0);
}

TEST_CASE("NORMAL is cleared for directories", "[permissions][attributes]")
{
    uint32_t attrs = ext4_mode_to_win_attributes(0755, true, "dir");
    CHECK((attrs & FILE_ATTRIBUTE_DIRECTORY) != 0);
    CHECK((attrs & FILE_ATTRIBUTE_NORMAL) == 0);
}

// ============================================================
//  Combined attributes
// ============================================================

TEST_CASE("Hidden readonly file has both flags", "[permissions][combined]")
{
    uint32_t attrs = ext4_mode_to_win_attributes(0444, false, ".secret");
    CHECK((attrs & FILE_ATTRIBUTE_HIDDEN) != 0);
    CHECK((attrs & FILE_ATTRIBUTE_READONLY) != 0);
    CHECK((attrs & FILE_ATTRIBUTE_NORMAL) == 0);
}

TEST_CASE("Hidden directory is hidden but not readonly", "[permissions][combined]")
{
    uint32_t attrs = ext4_mode_to_win_attributes(0555, true, ".config");
    CHECK((attrs & FILE_ATTRIBUTE_HIDDEN) != 0);
    CHECK((attrs & FILE_ATTRIBUTE_DIRECTORY) != 0);
    CHECK((attrs & FILE_ATTRIBUTE_READONLY) == 0);
}

// ============================================================
//  All common Linux permission patterns
// ============================================================

TEST_CASE("Common permission patterns", "[permissions][patterns]")
{
    struct TestCase {
        uint32_t mode;
        bool is_dir;
        const char* name;
        bool expect_readonly;
        bool expect_hidden;
    };

    TestCase cases[] = {
        // mode   dir?   name           readonly?  hidden?
        { 0777,  false, "full.txt",     false,     false  },
        { 0755,  false, "exec.sh",      false,     false  },
        { 0644,  false, "normal.txt",   false,     false  },
        { 0600,  false, "private.key",  false,     false  },
        { 0444,  false, "readonly.txt", true,      false  },
        { 0555,  false, "roexec",       true,      false  },
        { 0000,  false, "noperm",       true,      false  },
        { 0644,  false, ".bashrc",      false,     true   },
        { 0444,  false, ".secret",      true,      true   },
        { 0755,  true,  "home",         false,     false  },
        { 0700,  true,  ".ssh",         false,     true   },
        { 0555,  true,  "mount",        false,     false  },
        { 0775,  true,  "shared",       false,     false  },
    };

    for (auto& tc : cases) {
        INFO("mode=" << std::oct << tc.mode << " dir=" << tc.is_dir
             << " name=" << tc.name);
        uint32_t attrs = ext4_mode_to_win_attributes(tc.mode, tc.is_dir, tc.name);

        if (tc.expect_readonly) {
            CHECK((attrs & FILE_ATTRIBUTE_READONLY) != 0);
        } else {
            CHECK((attrs & FILE_ATTRIBUTE_READONLY) == 0);
        }

        if (tc.expect_hidden) {
            CHECK((attrs & FILE_ATTRIBUTE_HIDDEN) != 0);
        } else {
            CHECK((attrs & FILE_ATTRIBUTE_HIDDEN) == 0);
        }
    }
}
