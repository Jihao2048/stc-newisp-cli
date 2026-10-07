#include "newisp/chip_table.h"

namespace {
    // Returned for chip IDs that are not in the table.
    constexpr const char* kUnknown = "未知型号";
}

const char* const kUnknownChipName = kUnknown;

std::string ChipNameFromTable(uint16_t magic)
{
    auto it = kChipTable.find(magic);
    if (it == kChipTable.end()) return std::string(kUnknown);
    return std::string(it->second);
}
