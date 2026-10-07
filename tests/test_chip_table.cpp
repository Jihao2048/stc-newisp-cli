// Chip table lookup tests.

#include "test.h"

#include "newisp/chip_table.h"

namespace {

    void TestKnownParts()
    {
        // A representative part from each family the table covers. These IDs
        // are the ones the protocol code keys its behaviour on.
        CHECK_STREQ(ChipNameFromTable(0xFFFF), "STC89C516RD");
        CHECK_STREQ(ChipNameFromTable(0xF8B4), "STC8051U34K64");
        CHECK_STREQ(ChipNameFromTable(0xF743), "STC8H3K60S4");
        CHECK_STREQ(ChipNameFromTable(0x7701), "AI8C1K02");
        CHECK_STREQ(ChipNameFromTable(0xF8D1), "STC32G144K246");
    }

    void TestUnknownIdGetsPlaceholder()
    {
        // 0x0000 is not a real chip ID. The caller distinguishes "unknown" by
        // comparing against kUnknownChipName rather than by an empty string, so
        // the placeholder must be non-empty and stable.
        std::string name = ChipNameFromTable(0x0000);
        CHECK(!name.empty());
        CHECK_STREQ(name, kUnknownChipName);

        // A plausible-looking but absent ID must take the same path.
        CHECK_STREQ(ChipNameFromTable(0x0001), kUnknownChipName);
    }

    void TestTableIsNotEmpty()
    {
        CHECK(kChipTable.size() > 1000);
    }

    void TestNoNullNames()
    {
        // Every entry must be a real name: a null pointer would be turned into
        // a std::string by the lookup and crash.
        for (const auto& entry : kChipTable) {
            if (entry.second == nullptr) {
                ::newisp_test::Report(__FILE__, __LINE__,
                    "null name for chip id " + std::to_string(entry.first));
                continue;
            }
            if (entry.second[0] == '\0') {
                ::newisp_test::Report(__FILE__, __LINE__,
                    "empty name for chip id " + std::to_string(entry.first));
            }
        }
    }

} // namespace

void RunChipTableTests()
{
    TestKnownParts();
    TestUnknownIdGetsPlaceholder();
    TestTableIsNotEmpty();
    TestNoNullNames();
}
