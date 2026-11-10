#include "LrcParser.h"

#define NOMINMAX
#include <Windows.h>

#include <algorithm>
#include <charconv>
#include <fstream>
#include <sstream>

namespace {
std::string Trim(std::string value)
{
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}
}

std::vector<LyricLine> ParseLrc(const std::string& contents)
{
    std::vector<LyricLine> lines;
    std::istringstream input(contents);
    std::string row;
    bool firstRow = true;
    while (std::getline(input, row)) {
        if (firstRow && row.size() >= 3 &&
            static_cast<unsigned char>(row[0]) == 0xEF &&
            static_cast<unsigned char>(row[1]) == 0xBB &&
            static_cast<unsigned char>(row[2]) == 0xBF) {
            row.erase(0, 3);
        }
        firstRow = false;
        std::vector<std::chrono::milliseconds> timestamps;
        std::size_t cursor = 0;
        while (cursor + 1 < row.size() && row[cursor] == '[') {
            const auto close = row.find(']', cursor + 1);
            if (close == std::string::npos) {
                break;
            }
            const auto colon = row.find(':', cursor + 1);
            const auto dot = row.find('.', colon == std::string::npos ? close : colon + 1);
            if (colon == std::string::npos || colon >= close || dot == std::string::npos || dot >= close) {
                break;
            }
            int minutes = 0;
            int seconds = 0;
            int centiseconds = 0;
            auto parse = [](const char* begin, const char* end, int& result) {
                const auto parsed = std::from_chars(begin, end, result);
                return parsed.ec == std::errc{} && parsed.ptr == end;
            };
            if (!parse(row.data() + cursor + 1, row.data() + colon, minutes) ||
                !parse(row.data() + colon + 1, row.data() + dot, seconds) ||
                !parse(row.data() + dot + 1, row.data() + close, centiseconds) ||
                seconds >= 60 || centiseconds > 99) {
                break;
            }
            timestamps.emplace_back(std::chrono::minutes(minutes) +
                                    std::chrono::seconds(seconds) +
                                    std::chrono::milliseconds(centiseconds * 10));
            cursor = close + 1;
        }
        const auto text = Trim(row.substr(cursor));
        for (const auto timestamp : timestamps) {
            lines.push_back({timestamp, text});
        }
    }
    std::sort(lines.begin(), lines.end(), [](const auto& left, const auto& right) {
        return left.timestamp < right.timestamp;
    });
    return lines;
}

std::string ReadUtf8File(const std::wstring& path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return {};
    }
    std::ostringstream contents;
    contents << file.rdbuf();
    return contents.str();
}
