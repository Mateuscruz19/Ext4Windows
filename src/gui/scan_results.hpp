#pragma once

#include <istream>
#include <string>
#include <vector>

struct ScanResult {
    std::string path;
    std::string name;
};

// The elevated scanner writes UTF-8 with a BOM and additional fields.
// The non-elevated GUI also writes a two-field file without a BOM.
inline std::vector<ScanResult> read_scan_results(std::istream& input)
{
    std::vector<ScanResult> results;
    std::string line;
    bool first_line = true;
    while (std::getline(input, line)) {
        if (first_line && line.compare(0, 3, "\xEF\xBB\xBF") == 0)
            line.erase(0, 3);
        first_line = false;
        if (!line.empty() && line.back() == '\r')
            line.pop_back();

        const auto sep = line.find('|');
        if (sep == std::string::npos || sep == 0)
            continue;
        const auto next = line.find('|', sep + 1);
        results.push_back({line.substr(0, sep), line.substr(sep + 1, next - sep - 1)});
    }
    return results;
}
