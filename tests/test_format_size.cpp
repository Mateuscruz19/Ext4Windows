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

// Test Suite: Size Formatting
// Tests human-readable byte size formatting (used in partition scanner UI).

#include <catch_amalgamated.hpp>
#include "test_utils.hpp"

using namespace ext4windows;

// ============================================================
//  Bytes range (< 1 MB)
// ============================================================

TEST_CASE("format_size: zero bytes", "[format]")
{
    CHECK(format_size(0) == L"0 bytes");
}

TEST_CASE("format_size: single byte", "[format]")
{
    CHECK(format_size(1) == L"1 bytes");
}

TEST_CASE("format_size: 512 bytes (one sector)", "[format]")
{
    CHECK(format_size(512) == L"512 bytes");
}

TEST_CASE("format_size: 1023 bytes (just under 1KB)", "[format]")
{
    CHECK(format_size(1023) == L"1023 bytes");
}

TEST_CASE("format_size: 1024 bytes (1KB) still in bytes", "[format]")
{
    // format_size doesn't have KB tier, so 1024 is still "bytes"
    CHECK(format_size(1024) == L"1024 bytes");
}

TEST_CASE("format_size: values under 1 MB stay in bytes", "[format]")
{
    CHECK(format_size(1048575) == L"1048575 bytes");  // 1MB - 1
}

// ============================================================
//  Megabytes range (1 MB to 1 GB)
// ============================================================

TEST_CASE("format_size: exactly 1 MB", "[format]")
{
    CHECK(format_size(1048576) == L"1.00 MB");
}

TEST_CASE("format_size: 1.5 MB", "[format]")
{
    CHECK(format_size(1572864) == L"1.50 MB");
}

TEST_CASE("format_size: 100 MB", "[format]")
{
    CHECK(format_size(104857600) == L"100.00 MB");
}

TEST_CASE("format_size: 512 MB", "[format]")
{
    CHECK(format_size(536870912) == L"512.00 MB");
}

TEST_CASE("format_size: 999.99 MB (just under 1 GB)", "[format]")
{
    // 1073741823 bytes = 1 GB - 1 byte, should still be MB
    std::wstring result = format_size(1073741823);
    CHECK(result.find(L"MB") != std::wstring::npos);
}

// ============================================================
//  Gigabytes range (1 GB to 1 TB)
// ============================================================

TEST_CASE("format_size: exactly 1 GB", "[format]")
{
    CHECK(format_size(1073741824) == L"1.00 GB");
}

TEST_CASE("format_size: 2.5 GB", "[format]")
{
    CHECK(format_size(2684354560ULL) == L"2.50 GB");
}

TEST_CASE("format_size: 50 GB (typical Linux partition)", "[format]")
{
    CHECK(format_size(53687091200ULL) == L"50.00 GB");
}

TEST_CASE("format_size: 500 GB", "[format]")
{
    CHECK(format_size(536870912000ULL) == L"500.00 GB");
}

TEST_CASE("format_size: 999 GB", "[format]")
{
    std::wstring result = format_size(1072668082176ULL);
    CHECK(result.find(L"GB") != std::wstring::npos);
}

// ============================================================
//  Terabytes range (>= 1 TB)
// ============================================================

TEST_CASE("format_size: exactly 1 TB", "[format]")
{
    CHECK(format_size(1099511627776ULL) == L"1.00 TB");
}

TEST_CASE("format_size: 2 TB", "[format]")
{
    CHECK(format_size(2199023255552ULL) == L"2.00 TB");
}

TEST_CASE("format_size: 4 TB (common disk size)", "[format]")
{
    CHECK(format_size(4398046511104ULL) == L"4.00 TB");
}

TEST_CASE("format_size: 16 TB", "[format]")
{
    CHECK(format_size(17592186044416ULL) == L"16.00 TB");
}

// ============================================================
//  GUID comparison
// ============================================================

TEST_CASE("guid_equal: identical GUIDs", "[guid]")
{
    // Linux filesystem GUID: 0FC63DAF-8483-4772-8E79-3D69D8477DE4
    GUID linux_fs = {0x0FC63DAF, 0x8483, 0x4772,
                     {0x8E, 0x79, 0x3D, 0x69, 0xD8, 0x47, 0x7D, 0xE4}};
    GUID linux_fs2 = linux_fs;
    CHECK(guid_equal(linux_fs, linux_fs2) == true);
}

TEST_CASE("guid_equal: different GUIDs", "[guid]")
{
    GUID a = {0x0FC63DAF, 0x8483, 0x4772,
              {0x8E, 0x79, 0x3D, 0x69, 0xD8, 0x47, 0x7D, 0xE4}};
    GUID b = {0xEBD0A0A2, 0xB9E5, 0x4433,
              {0x87, 0xC0, 0x68, 0xB6, 0xB7, 0x26, 0x99, 0xC7}};
    CHECK(guid_equal(a, b) == false);
}

TEST_CASE("guid_equal: zero GUIDs", "[guid]")
{
    GUID zero1 = {};
    GUID zero2 = {};
    CHECK(guid_equal(zero1, zero2) == true);
}

TEST_CASE("guid_equal: differs in one byte", "[guid]")
{
    GUID a = {0x0FC63DAF, 0x8483, 0x4772,
              {0x8E, 0x79, 0x3D, 0x69, 0xD8, 0x47, 0x7D, 0xE4}};
    GUID b = a;
    b.Data4[7] = 0xE5;  // change last byte
    CHECK(guid_equal(a, b) == false);
}
