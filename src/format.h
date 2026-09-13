#pragma once

#include <cstdint>
#include <cwchar>
#include <string>

inline std::wstring FormatSize(uint64_t bytes) {
    static const wchar_t* kUnits[] = {L"B", L"KB", L"MB", L"GB", L"TB", L"PB"};
    wchar_t buf[64];

    if (bytes < 1024) {
        swprintf_s(buf, L"%llu B", static_cast<unsigned long long>(bytes));
        return buf;
    }

    double value = static_cast<double>(bytes);
    int unit = 0;
    while (value >= 1024.0 && unit < 5) {
        value /= 1024.0;
        ++unit;
    }
    swprintf_s(buf, L"%.2f %s", value, kUnits[unit]);
    return buf;
}
