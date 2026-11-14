#pragma once

#include <Windows.h>
#include <ShlObj.h>
#include <filesystem>
#include <string>

namespace pluginfiles {
inline std::filesystem::path EnvironmentPath(const wchar_t* name)
{
    const DWORD size = GetEnvironmentVariableW(name, nullptr, 0);
    if (!size) return {};
    std::wstring value(size, L'\0');
    const DWORD copied = GetEnvironmentVariableW(name, value.data(), size);
    if (!copied || copied >= size) return {};
    value.resize(copied);
    return value;
}

inline std::filesystem::path ConfigFile(const wchar_t* plugin, const wchar_t* filename)
{
    // Use an optional portable data root instead of Windows Documents.
    const auto portable = EnvironmentPath(L"RESOLUME_PLUGIN_DATA_DIR");
    if (!portable.empty()) return portable / plugin / filename;

    PWSTR documents = nullptr;
    std::filesystem::path current;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, KF_FLAG_DEFAULT, nullptr, &documents))) {
        current = std::filesystem::path(documents) / L"Resolume" / plugin / filename;
    }
    CoTaskMemFree(documents);
    const auto profile = EnvironmentPath(L"USERPROFILE");
    const auto legacy = profile.empty() ? std::filesystem::path{} :
        profile / L"Documents" / L"Resolume" / plugin / filename;
    std::error_code error;
    if (!current.empty() && std::filesystem::exists(current, error)) return current;
    error.clear();
    if (!legacy.empty() && std::filesystem::exists(legacy, error)) return legacy;
    return current.empty() ? legacy : current;
}
}
