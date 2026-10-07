#pragma once
// Cross-platform text and timing helpers.
//
// The original WinUI build used std::wstring throughout because every string
// ended up in a XAML control. A console tool has no such requirement: it writes
// UTF-8 to stdout, and on Linux/macOS wchar_t is 4 bytes wide, which makes
// std::wstring both wasteful and awkward to print. This port therefore uses
// std::string holding UTF-8 everywhere, including the Chinese log messages.
//
// The only remaining wide-string use is the Win32 serial/HID API layer, which
// converts at the boundary; see src/transport.

#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <string>
#include <vector>
#include <chrono>
#include <thread>

namespace newisp {

// Monotonic millisecond counter, equivalent to the Win32 GetTickCount64() the
// original code used. std::chrono::steady_clock is the portable spelling and
// cannot go backwards when the wall clock is adjusted.
inline uint64_t NowMs()
{
    using namespace std::chrono;
    return (uint64_t)duration_cast<milliseconds>(
        steady_clock::now().time_since_epoch()).count();
}

inline void SleepMs(uint32_t ms)
{
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

// Uppercase hex of a value, zero padded to `width` digits.
inline std::string Hex(uint32_t v, int width = 2)
{
    static const char* H = "0123456789ABCDEF";
    std::string s((size_t)width, '0');
    for (int i = width - 1; i >= 0; --i) {
        s[(size_t)i] = H[v & 0xF];
        v >>= 4;
    }
    return s;
}

// "%02X %02X ..." dump, wrapped after 16 bytes, matching the original HexDump.
inline std::string HexDump(const uint8_t* data, size_t size)
{
    static const char* H = "0123456789ABCDEF";
    std::string s;
    s.reserve(size * 3 + 16);
    for (size_t i = 0; i < size; ++i) {
        s += H[(data[i] >> 4) & 0xF];
        s += H[data[i] & 0xF];
        s += ' ';
        if ((i + 1) % 16 == 0) s += "\r\n         ";
    }
    return s;
}

inline std::string HexDump(const std::vector<uint8_t>& data)
{
    return HexDump(data.data(), data.size());
}

// HexDump of an initializer list, which the protocol layer uses to echo the MSR
// bytes it is about to write back.
inline std::string HexDump(std::initializer_list<uint8_t> data)
{
    return HexDump(data.begin(), data.size());
}

// Decimal formatting with an explicit precision, replacing swprintf_s.
inline std::string FormatDouble(double v, int decimals)
{
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.*f", decimals, v);
    return std::string(buf);
}

} // namespace newisp
