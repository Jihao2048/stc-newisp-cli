#pragma once
// Intel HEX parser.
//
// Ported unchanged from the WinUI build apart from the include path: this
// parser never touched a platform API in the first place.

#include <string>
#include <vector>
#include <cstdint>

namespace stc {

    struct HexImage {
        std::vector<uint8_t> data;
        uint32_t minAddr = 0xFFFFFFFF;
        uint32_t maxAddr = 0;
    };

    // Parse Intel HEX text into a flat image. Returns false when no data
    // records were found (which also covers an all-comment or empty file).
    bool ParseHexFile(const std::string& text, HexImage& out);

    // Read a file from disk and parse it. Returns false if the file cannot be
    // read or contains no data records.
    bool LoadHexFile(const std::string& path, HexImage& out, std::string& error);

} // namespace stc
