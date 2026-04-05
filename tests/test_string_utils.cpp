// Test Suite: String Utilities
// Tests all string conversion and path manipulation functions.

#include <catch_amalgamated.hpp>
#include "test_utils.hpp"

using namespace ext4windows;

// ============================================================
//  Wide String to UTF-8 Conversion
// ============================================================

TEST_CASE("wide_to_utf8 converts basic ASCII", "[string][utf8]")
{
    CHECK(wide_to_utf8(L"hello") == "hello");
    CHECK(wide_to_utf8(L"Hello World") == "Hello World");
    CHECK(wide_to_utf8(L"C:\\Users\\test") == "C:\\Users\\test");
}

TEST_CASE("wide_to_utf8 converts special characters", "[string][utf8]")
{
    // Use \uXXXX escape for wide chars to avoid source encoding issues
    CHECK(wide_to_utf8(L"caf\u00E9") == std::string("caf\xc3\xa9"));       // é
    CHECK(wide_to_utf8(L"na\u00EFve") == std::string("na\xc3\xaf" "ve"));  // ï
    CHECK(wide_to_utf8(L"\u00FC" L"ber") == std::string("\xc3\xbc" "ber")); // ü
}

TEST_CASE("wide_to_utf8 converts CJK characters", "[string][utf8]")
{
    // 日本語 = 3 CJK chars, each 3 bytes in UTF-8
    std::string result = wide_to_utf8(L"\u65E5\u672C\u8A9E");
    CHECK(result.size() == 9);  // 3 chars * 3 bytes each
}

TEST_CASE("wide_to_utf8 handles empty and null", "[string][utf8]")
{
    CHECK(wide_to_utf8(L"") == "");
    CHECK(wide_to_utf8(nullptr) == "");
}

TEST_CASE("wide_to_utf8 handles paths with spaces", "[string][utf8]")
{
    CHECK(wide_to_utf8(L"C:\\Program Files\\Ext4Windows") == "C:\\Program Files\\Ext4Windows");
}

// ============================================================
//  UTF-8 to Wide String Conversion
// ============================================================

TEST_CASE("utf8_to_wide converts basic ASCII", "[string][wide]")
{
    CHECK(utf8_to_wide("hello") == L"hello");
    CHECK(utf8_to_wide("test.img") == L"test.img");
}

TEST_CASE("utf8_to_wide handles empty and null", "[string][wide]")
{
    CHECK(utf8_to_wide("") == L"");
    CHECK(utf8_to_wide(nullptr) == L"");
}

TEST_CASE("utf8_to_wide roundtrips with wide_to_utf8", "[string][roundtrip]")
{
    const wchar_t* test_strings[] = {
        L"hello",
        L"C:\\Users\\Mateus\\test.img",
        L"path with spaces",
        L"café",
        L"\\\\.\\PhysicalDrive0",
    };

    for (const wchar_t* ws : test_strings) {
        INFO("Testing: " << wide_to_utf8(ws));
        CHECK(utf8_to_wide(wide_to_utf8(ws).c_str()) == std::wstring(ws));
    }
}

// ============================================================
//  Windows Path to ext4 Path
// ============================================================

TEST_CASE("to_ext4_path converts root path", "[path][ext4]")
{
    CHECK(to_ext4_path(L"\\", "/mnt_Z/") == "/mnt_Z/");
    CHECK(to_ext4_path(L"/", "/mnt_Z/") == "/mnt_Z/");
}

TEST_CASE("to_ext4_path converts backslashes to forward slashes", "[path][ext4]")
{
    CHECK(to_ext4_path(L"\\docs\\readme.txt", "/mnt_Z/") == "/mnt_Z/docs/readme.txt");
    CHECK(to_ext4_path(L"\\home\\user\\file", "/mnt_Z/") == "/mnt_Z/home/user/file");
}

TEST_CASE("to_ext4_path handles files in root", "[path][ext4]")
{
    CHECK(to_ext4_path(L"\\hello.txt", "/mnt_Z/") == "/mnt_Z/hello.txt");
    CHECK(to_ext4_path(L"\\lost+found", "/mnt_Z/") == "/mnt_Z/lost+found");
}

TEST_CASE("to_ext4_path handles different mount points", "[path][ext4]")
{
    CHECK(to_ext4_path(L"\\file.txt", "/mnt_A/") == "/mnt_A/file.txt");
    CHECK(to_ext4_path(L"\\file.txt", "/mnt_Y/") == "/mnt_Y/file.txt");
}

TEST_CASE("to_ext4_path handles deep paths", "[path][ext4]")
{
    CHECK(to_ext4_path(L"\\a\\b\\c\\d\\e\\f.txt", "/mnt_Z/")
          == "/mnt_Z/a/b/c/d/e/f.txt");
}

TEST_CASE("to_ext4_path handles paths without leading backslash", "[path][ext4]")
{
    CHECK(to_ext4_path(L"file.txt", "/mnt_Z/") == "/mnt_Z/file.txt");
    CHECK(to_ext4_path(L"dir/file.txt", "/mnt_Z/") == "/mnt_Z/dir/file.txt");
}

TEST_CASE("to_ext4_path handles empty path", "[path][ext4]")
{
    // Empty becomes "/" which returns mount point
    std::string result = to_ext4_path(L"", "/mnt_Z/");
    // Empty wide string → empty UTF-8 → "/" prefix → "/" → returns mount_point
    CHECK(result == "/mnt_Z/");
}

TEST_CASE("to_ext4_path handles special characters in filenames", "[path][ext4]")
{
    CHECK(to_ext4_path(L"\\file with spaces.txt", "/mnt_Z/")
          == "/mnt_Z/file with spaces.txt");
    CHECK(to_ext4_path(L"\\.hidden", "/mnt_Z/") == "/mnt_Z/.hidden");
    CHECK(to_ext4_path(L"\\file-name_v2.0.tar.gz", "/mnt_Z/")
          == "/mnt_Z/file-name_v2.0.tar.gz");
}
