#pragma once
#include <cctype>
#include <string>

// Read JSON string tokens without depending on response whitespace or field order.
inline bool ReadJsonString(const std::string& json, std::size_t& cursor, std::string& value)
{
    if (cursor >= json.size() || json[cursor++] != '"') return false;
    auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    auto readHex = [&](unsigned& code) {
        code = 0;
        for (int i = 0; i < 4; ++i) {
            if (cursor >= json.size()) return false;
            const int digit = hex(json[cursor++]);
            if (digit < 0) return false;
            code = code * 16 + digit;
        }
        return true;
    };
    while (cursor < json.size()) {
        const char c = json[cursor++];
        if (c == '"') return true;
        if (static_cast<unsigned char>(c) < 0x20) return false;
        if (c != '\\') { value += c; continue; }
        if (cursor >= json.size()) return false;
        switch (json[cursor++]) {
        case '"': value += '"'; break;
        case '\\': value += '\\'; break;
        case '/': value += '/'; break;
        case 'n': value += '\n'; break;
        case 'r': value += '\r'; break;
        case 't': value += '\t'; break;
        case 'b': value += '\b'; break;
        case 'f': value += '\f'; break;
        case 'u': {
            unsigned code;
            if (!readHex(code)) return false;
            if (code >= 0xD800 && code <= 0xDBFF) {
                if (cursor + 2 > json.size() || json[cursor++] != '\\' || json[cursor++] != 'u') return false;
                unsigned low;
                if (!readHex(low) || low < 0xDC00 || low > 0xDFFF) return false;
                code = 0x10000 + ((code - 0xD800) << 10) + low - 0xDC00;
            } else if (code >= 0xDC00 && code <= 0xDFFF) return false;
            if (code <= 0x7F) {
                value += static_cast<char>(code);
            } else if (code <= 0x7FF) {
                value += static_cast<char>(0xC0 | (code >> 6));
                value += static_cast<char>(0x80 | (code & 0x3F));
            } else if (code <= 0xFFFF) {
                value += static_cast<char>(0xE0 | (code >> 12));
                value += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
                value += static_cast<char>(0x80 | (code & 0x3F));
            } else {
                value += static_cast<char>(0xF0 | (code >> 18));
                value += static_cast<char>(0x80 | ((code >> 12) & 0x3F));
                value += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
                value += static_cast<char>(0x80 | (code & 0x3F));
            }
            break;
        }
        default: return false;
        }
    }
    return false;
}

inline std::string ExtractSyncedLyrics(const std::string& json)
{
    for (std::size_t cursor = 0; cursor < json.size();) {
        if (json[cursor] != '"') { ++cursor; continue; }
        std::string key;
        if (!ReadJsonString(json, cursor, key)) return {};
        if (key != "syncedLyrics") continue;
        while (cursor < json.size() && std::isspace(static_cast<unsigned char>(json[cursor]))) ++cursor;
        if (cursor == json.size() || json[cursor++] != ':') continue;
        while (cursor < json.size() && std::isspace(static_cast<unsigned char>(json[cursor]))) ++cursor;
        if (cursor == json.size() || json[cursor] != '"') continue;
        std::string value;
        if (!ReadJsonString(json, cursor, value)) return {};
        if (!value.empty()) return value;
    }
    return {};
}
