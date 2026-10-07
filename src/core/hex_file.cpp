#include "newisp/hex_file.h"

#include <cstdio>
#include <fstream>
#include <sstream>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace stc {

    bool ParseHexFile(const std::string& text, HexImage& out)
    {
        uint32_t base = 0;
        auto nib = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            return -1;
            };
        auto hb = [&](const std::string& s, size_t o) -> int {
            if (o + 1 >= s.size()) return -1;
            int hi = nib(s[o]), lo = nib(s[o + 1]);
            return (hi < 0 || lo < 0) ? -1 : ((hi << 4) | lo);
            };

        size_t start = 0;
        while (start < text.size()) {
            size_t end = text.find('\n', start);
            if (end == std::string::npos) end = text.size();
            std::string line = text.substr(start, end - start);
            start = end + 1;
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
            if (line.empty() || line[0] != ':') continue;

            int bc = hb(line, 1), ah = hb(line, 3), al = hb(line, 5), rt = hb(line, 7);
            if (bc < 0 || ah < 0 || al < 0 || rt < 0) continue;
            // Both halves were just bounds-checked to 0..255 by hb(), so the
            // cast to uint16_t cannot truncate; be explicit about it.
            uint16_t addr = (uint16_t)(((uint16_t)ah << 8) | (uint16_t)al);

            if (rt == 0x04) {
                int hi = hb(line, 9), lo = hb(line, 11);
                if (hi >= 0 && lo >= 0) base = ((uint32_t)((hi << 8) | lo)) << 16; continue;
            }
            if (rt == 0x02) {
                int hi = hb(line, 9), lo = hb(line, 11);
                if (hi >= 0 && lo >= 0) base = ((uint32_t)((hi << 8) | lo)) << 4; continue;
            }
            if (rt == 0x01) break;
            if (rt != 0x00) continue;

            uint32_t abs = base + addr;
            for (int i = 0; i < bc; ++i) {
                int b = hb(line, 9 + i * 2);
                if (b < 0) break;
                uint32_t a = abs + i;
                if (a + 1 > out.data.size()) out.data.resize(a + 1, 0xFF);
                out.data[a] = (uint8_t)b;
                if (a < out.minAddr) out.minAddr = a;
                if (a > out.maxAddr) out.maxAddr = a;
            }
        }
        return out.maxAddr >= out.minAddr;
    }

    bool LoadHexFile(const std::string& path, HexImage& out, std::string& error)
    {
        std::string text;

#if defined(_WIN32)
        // std::ifstream takes a narrow path and interprets it in the system
        // ANSI code page, so a path containing non-ASCII characters would be
        // looked up wrongly. The same path arrives here as UTF-8, so convert it
        // to UTF-16 and use the wide stream, which is unambiguous.
        int wlen = MultiByteToWideChar(CP_UTF8, 0, path.c_str(),
            (int)path.size(), nullptr, 0);
        if (wlen <= 0) {
            error = "cannot convert path to UTF-16: " + path;
            return false;
        }
        std::wstring wpath((size_t)wlen, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, path.c_str(), (int)path.size(),
            &wpath[0], wlen);

        std::ifstream in(wpath.c_str(), std::ios::binary);
        if (!in) {
            error = "cannot open file: " + path;
            return false;
        }
        std::ostringstream ss;
        ss << in.rdbuf();
        text = ss.str();
#else
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            error = "cannot open file: " + path;
            return false;
        }
        std::ostringstream ss;
        ss << in.rdbuf();
        text = ss.str();
#endif

        if (!ParseHexFile(text, out)) {
            error = "no data records found in " + path;
            return false;
        }
        return true;
    }

} // namespace stc
