#include <catch_amalgamated.hpp>
#include "gui/scan_results.hpp"
#include <sstream>

TEST_CASE("scan results preserve device paths and display names", "[scan]")
{
    std::istringstream input(
        "\xEF\xBB\xBF\\\\?\\GLOBALROOT\\Device\\Harddisk4\\Partition5|Disk 4, Partition 5|393894428672|4|5\r\n"
        "\\\\?\\GLOBALROOT\\Device\\Harddisk6\\Partition1|Disk 6, Partition 1|120032591872|6|1\r\n");
    const auto results = read_scan_results(input);
    REQUIRE(results.size() == 2);
    CHECK(results[0].path == "\\\\?\\GLOBALROOT\\Device\\Harddisk4\\Partition5");
    CHECK(results[0].name == "Disk 4, Partition 5");
    CHECK(results[1].path == "\\\\?\\GLOBALROOT\\Device\\Harddisk6\\Partition1");
    CHECK(results[1].name == "Disk 6, Partition 1");
}

TEST_CASE("scan results accept the non-elevated two-field format", "[scan]")
{
    std::istringstream input("device|display name\ninvalid\n|missing path\n");
    const auto results = read_scan_results(input);
    REQUIRE(results.size() == 1);
    CHECK(results[0].path == "device");
    CHECK(results[0].name == "display name");
}
