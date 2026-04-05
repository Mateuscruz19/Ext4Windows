// Test Suite: Timestamp Conversion
// Tests POSIX ↔ Windows FILETIME conversion functions.
// Critical for correct file dates in Explorer.

#include <catch_amalgamated.hpp>
#include "test_utils.hpp"

using namespace ext4windows;

// ============================================================
//  POSIX to Windows FILETIME
// ============================================================

TEST_CASE("posix_to_windows_filetime converts epoch zero", "[timestamp]")
{
    // POSIX 0 = 1970-01-01 00:00:00 UTC
    // Windows FILETIME for that = 116444736000000000
    uint64_t result = posix_to_windows_filetime(0);
    CHECK(result == 116444736000000000ULL);
}

TEST_CASE("posix_to_windows_filetime converts known dates", "[timestamp]")
{
    // 2000-01-01 00:00:00 UTC = POSIX 946684800
    uint64_t y2k = posix_to_windows_filetime(946684800);
    // Expected: 946684800 * 10000000 + 116444736000000000
    //         = 9466848000000000 + 116444736000000000
    //         = 125911584000000000
    CHECK(y2k == 125911584000000000ULL);
}

TEST_CASE("posix_to_windows_filetime converts recent date", "[timestamp]")
{
    // 2024-01-01 00:00:00 UTC = POSIX 1704067200
    uint64_t result = posix_to_windows_filetime(1704067200);
    uint64_t expected = static_cast<uint64_t>(1704067200) * 10000000ULL + 116444736000000000ULL;
    CHECK(result == expected);
}

TEST_CASE("posix_to_windows_filetime handles max 32-bit timestamp", "[timestamp]")
{
    // Max uint32 = 4294967295 = 2106-02-07 06:28:15 UTC
    uint64_t result = posix_to_windows_filetime(0xFFFFFFFF);
    uint64_t expected = static_cast<uint64_t>(0xFFFFFFFF) * 10000000ULL + 116444736000000000ULL;
    CHECK(result == expected);
    // Must not overflow: result should be larger than epoch diff
    CHECK(result > 116444736000000000ULL);
}

TEST_CASE("posix_to_windows_filetime is monotonically increasing", "[timestamp]")
{
    uint64_t t1 = posix_to_windows_filetime(1000);
    uint64_t t2 = posix_to_windows_filetime(2000);
    uint64_t t3 = posix_to_windows_filetime(3000);
    CHECK(t1 < t2);
    CHECK(t2 < t3);
}

TEST_CASE("posix_to_windows_filetime: 1 second = 10 million 100ns ticks", "[timestamp]")
{
    uint64_t t0 = posix_to_windows_filetime(100);
    uint64_t t1 = posix_to_windows_filetime(101);
    CHECK(t1 - t0 == 10000000ULL);  // exactly 1 second in 100ns units
}

// ============================================================
//  Windows FILETIME to POSIX
// ============================================================

TEST_CASE("windows_filetime_to_posix converts epoch", "[timestamp]")
{
    uint32_t result = windows_filetime_to_posix(116444736000000000ULL);
    CHECK(result == 0);
}

TEST_CASE("windows_filetime_to_posix converts Y2K", "[timestamp]")
{
    uint32_t result = windows_filetime_to_posix(125911584000000000ULL);
    CHECK(result == 946684800);
}

TEST_CASE("windows_filetime_to_posix handles pre-epoch times", "[timestamp]")
{
    // Times before 1970 should return 0 (not underflow)
    uint32_t result = windows_filetime_to_posix(0);
    CHECK(result == 0);

    result = windows_filetime_to_posix(100000000ULL);  // way before 1970
    CHECK(result == 0);
}

// ============================================================
//  Roundtrip: POSIX → Windows → POSIX
// ============================================================

TEST_CASE("timestamp roundtrip preserves value", "[timestamp][roundtrip]")
{
    uint32_t test_values[] = {
        0,              // epoch
        1,              // 1 second after epoch
        946684800,      // Y2K
        1704067200,     // 2024-01-01
        0x7FFFFFFF,     // Y2K38 limit (2038-01-19)
        0xFFFFFFFF,     // max uint32 (2106)
    };

    for (uint32_t posix : test_values) {
        INFO("Testing POSIX timestamp: " << posix);
        uint64_t win = posix_to_windows_filetime(posix);
        uint32_t back = windows_filetime_to_posix(win);
        CHECK(back == posix);
    }
}

TEST_CASE("timestamp roundtrip: many sequential values", "[timestamp][roundtrip]")
{
    // Test 1000 sequential timestamps around a known date
    for (uint32_t t = 1700000000; t < 1700001000; t++) {
        uint64_t win = posix_to_windows_filetime(t);
        uint32_t back = windows_filetime_to_posix(win);
        CHECK(back == t);
    }
}
