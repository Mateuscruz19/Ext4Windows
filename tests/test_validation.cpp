// Ext4Windows — Mount ext4 partitions as Windows drive letters
// Copyright (C) 2026 Mateus Cruz
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License along
// with this program; if not, see <https://www.gnu.org/licenses/>.

// Test Suite: Input Validation & Security
// Tests all security-related validation functions to ensure
// the application rejects malicious inputs.

#include <catch_amalgamated.hpp>
#include "test_utils.hpp"

using namespace ext4windows;

// ============================================================
//  Drive Letter Validation
// ============================================================

TEST_CASE("is_valid_drive_letter accepts uppercase A-Z", "[validation][drive]")
{
    for (char c = 'A'; c <= 'Z'; c++) {
        INFO("Testing letter: " << c);
        CHECK(is_valid_drive_letter(c) == true);
    }
}

TEST_CASE("is_valid_drive_letter accepts lowercase a-z", "[validation][drive]")
{
    for (char c = 'a'; c <= 'z'; c++) {
        INFO("Testing letter: " << c);
        CHECK(is_valid_drive_letter(c) == true);
    }
}

TEST_CASE("is_valid_drive_letter rejects digits", "[validation][drive]")
{
    for (char c = '0'; c <= '9'; c++) {
        INFO("Testing digit: " << c);
        CHECK(is_valid_drive_letter(c) == false);
    }
}

TEST_CASE("is_valid_drive_letter rejects special characters", "[validation][drive]")
{
    const char specials[] = { '!', '@', '#', '$', '%', '^', '&', '*', '(', ')',
                              '-', '_', '=', '+', '[', ']', '{', '}', '|', '\\',
                              '/', '.', ',', '<', '>', '?', '`', '~', ' ', '\0',
                              '\t', '\n', '\r', 127 };
    for (char c : specials) {
        INFO("Testing char code: " << (int)c);
        CHECK(is_valid_drive_letter(c) == false);
    }
}

TEST_CASE("is_valid_drive_letter rejects characters just outside A-Z range", "[validation][drive]")
{
    CHECK(is_valid_drive_letter('@') == false);  // ASCII 64, just before 'A'
    CHECK(is_valid_drive_letter('[') == false);  // ASCII 91, just after 'Z'
    CHECK(is_valid_drive_letter('`') == false);  // ASCII 96, just before 'a'
    CHECK(is_valid_drive_letter('{') == false);  // ASCII 123, just after 'z'
}

// ============================================================
//  Path Traversal Detection
// ============================================================

TEST_CASE("path_contains_traversal detects '..' sequences", "[validation][security]")
{
    CHECK(path_contains_traversal("..") == true);
    CHECK(path_contains_traversal("../etc/passwd") == true);
    CHECK(path_contains_traversal("C:\\Users\\..\\System32") == true);
    CHECK(path_contains_traversal("/home/user/../../root") == true);
    CHECK(path_contains_traversal("a/b/../c") == true);
    CHECK(path_contains_traversal("file..name") == true);  // conservative: any ".."
}

TEST_CASE("path_contains_traversal detects null bytes", "[validation][security]")
{
    std::string with_null = "C:\\Users\\test";
    with_null += '\0';
    with_null += ".exe";
    CHECK(path_contains_traversal(with_null) == true);

    std::string null_at_start = std::string("\0", 1) + "evil";
    CHECK(path_contains_traversal(null_at_start) == true);
}

TEST_CASE("path_contains_traversal allows safe paths", "[validation][security]")
{
    CHECK(path_contains_traversal("C:\\Users\\admin\\file.img") == false);
    CHECK(path_contains_traversal("/home/user/disk.img") == false);
    CHECK(path_contains_traversal("D:\\linux\\ext4.img") == false);
    CHECK(path_contains_traversal("relative/path/to/file") == false);
    CHECK(path_contains_traversal("file.img") == false);
    CHECK(path_contains_traversal("") == false);
    CHECK(path_contains_traversal("a.b.c.d.e") == false);
    CHECK(path_contains_traversal("single.dot") == false);
}

TEST_CASE("path_contains_traversal handles edge cases", "[validation][security]")
{
    CHECK(path_contains_traversal(".") == false);   // single dot is fine
    CHECK(path_contains_traversal("...") == true);  // contains ".."
    CHECK(path_contains_traversal("....") == true);
    CHECK(path_contains_traversal("a..b") == true);
    CHECK(path_contains_traversal("..hidden") == true);
}

// ============================================================
//  Normalize Drive Letter
// ============================================================

TEST_CASE("normalize_drive_letter converts lowercase to uppercase", "[validation][drive]")
{
    CHECK(normalize_drive_letter(L'a') == L'A');
    CHECK(normalize_drive_letter(L'z') == L'Z');
    CHECK(normalize_drive_letter(L'm') == L'M');
}

TEST_CASE("normalize_drive_letter preserves uppercase", "[validation][drive]")
{
    CHECK(normalize_drive_letter(L'A') == L'A');
    CHECK(normalize_drive_letter(L'Z') == L'Z');
    CHECK(normalize_drive_letter(L'M') == L'M');
}

// ============================================================
//  Validate Mount Command
// ============================================================

TEST_CASE("validate_mount_command rejects empty source", "[validation][mount]")
{
    auto err = validate_mount_command("", "Z");
    CHECK_FALSE(err.empty());
    CHECK(err.find("ERROR") != std::string::npos);
}

TEST_CASE("validate_mount_command rejects empty drive", "[validation][mount]")
{
    auto err = validate_mount_command("C:\\test.img", "");
    CHECK_FALSE(err.empty());
}

TEST_CASE("validate_mount_command rejects invalid drive letter", "[validation][mount]")
{
    auto err = validate_mount_command("C:\\test.img", "1");
    CHECK(err.find("Invalid drive letter") != std::string::npos);
}

TEST_CASE("validate_mount_command rejects path traversal", "[validation][mount]")
{
    auto err = validate_mount_command("C:\\..\\Windows\\System32", "Z");
    CHECK(err.find("invalid sequences") != std::string::npos);
}

TEST_CASE("validate_mount_command accepts valid commands", "[validation][mount]")
{
    CHECK(validate_mount_command("C:\\linux.img", "Z").empty());
    CHECK(validate_mount_command("/home/user/disk.img", "A").empty());
    CHECK(validate_mount_command("test.img", "z").empty());
}

// ============================================================
//  Integer Overflow Guard
// ============================================================

TEST_CASE("would_overflow_dword detects overflow", "[validation][overflow]")
{
    // 0xFFFFFFFF * 512 would overflow
    CHECK(would_overflow_dword(0xFFFFFFFF, 512) == true);
    // Very large block count
    CHECK(would_overflow_dword(0x900000, 512) == true);
    // 2^32 / 512 = 8388608, anything above overflows
    CHECK(would_overflow_dword(8388609, 512) == true);
}

TEST_CASE("would_overflow_dword allows safe values", "[validation][overflow]")
{
    CHECK(would_overflow_dword(1, 512) == false);
    CHECK(would_overflow_dword(100, 512) == false);
    CHECK(would_overflow_dword(8388607, 512) == false);  // just under limit
    CHECK(would_overflow_dword(0, 512) == false);
}

TEST_CASE("would_overflow_dword boundary: exact limit", "[validation][overflow]")
{
    // 0xFFFFFFFF / 512 = 8388607
    CHECK(would_overflow_dword(8388607, 512) == false);
    CHECK(would_overflow_dword(8388608, 512) == true);
}

// ============================================================
//  Parse Command Action
// ============================================================

TEST_CASE("parse_command_action extracts first word", "[validation][protocol]")
{
    CHECK(parse_command_action("MOUNT C:\\test.img Z") == "MOUNT");
    CHECK(parse_command_action("UNMOUNT Z") == "UNMOUNT");
    CHECK(parse_command_action("STATUS") == "STATUS");
    CHECK(parse_command_action("QUIT") == "QUIT");
    CHECK(parse_command_action("SCAN") == "SCAN");
    CHECK(parse_command_action("MOUNT_PARTITION Z RO \\\\.\\Harddisk0Partition2") == "MOUNT_PARTITION");
}

TEST_CASE("parse_command_action handles edge cases", "[validation][protocol]")
{
    CHECK(parse_command_action("") == "");
    CHECK(parse_command_action("SINGLE") == "SINGLE");
    CHECK(parse_command_action("  leading_space") == "");  // first "word" is empty before space
}
