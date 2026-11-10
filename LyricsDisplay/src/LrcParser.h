#pragma once

#include <chrono>
#include <string>
#include <vector>

struct LyricLine {
    std::chrono::milliseconds timestamp{};
    std::string text;
};

std::vector<LyricLine> ParseLrc(const std::string& contents);
std::string ReadUtf8File(const std::wstring& path);
