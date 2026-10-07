// Intel HEX parser tests.

#include "test.h"

#include "newisp/hex_file.h"

#include <string>

using stc::HexImage;
using stc::ParseHexFile;

namespace {

    void TestSimpleImage()
    {
        // Four bytes at address 0x0100, then EOF.
        const std::string hex =
            ":0401000001020304F2\n"
            ":00000001FF\n";

        HexImage img;
        CHECK(ParseHexFile(hex, img));
        CHECK_EQ(img.minAddr, 0x0100u);
        CHECK_EQ(img.maxAddr, 0x0103u);
        CHECK_EQ(img.data.size(), (size_t)0x0104);
        CHECK_EQ((int)img.data[0x0100], 1);
        CHECK_EQ((int)img.data[0x0101], 2);
        CHECK_EQ((int)img.data[0x0102], 3);
        CHECK_EQ((int)img.data[0x0103], 4);

        // The gap below the first record must be filled with 0xFF, which is
        // what the flash is erased to; writing 0x00 there would corrupt a chip
        // that was only partially erased.
        CHECK_EQ((int)img.data[0], 0xFF);
    }

    void TestExtendedLinearAddress()
    {
        // Type 04 sets the upper 16 bits: base = 0x0001 << 16 = 0x00010000.
        const std::string hex =
            ":020000040001F9\n"
            ":04000000AABBCCDD52\n"
            ":00000001FF\n";

        HexImage img;
        CHECK(ParseHexFile(hex, img));
        CHECK_EQ(img.minAddr, 0x00010000u);
        CHECK_EQ(img.maxAddr, 0x00010003u);
        CHECK_EQ((int)img.data[0x00010000], 0xAA);
        CHECK_EQ((int)img.data[0x00010003], 0xDD);
    }

    void TestExtendedSegmentAddress()
    {
        // Type 02 sets the upper bits shifted left by 4: base = 0x1000 << 4.
        const std::string hex =
            ":020000021000EC\n"
            ":02000000123496\n"
            ":00000001FF\n";

        HexImage img;
        CHECK(ParseHexFile(hex, img));
        CHECK_EQ(img.minAddr, 0x00010000u);
        CHECK_EQ((int)img.data[0x00010000], 0x12);
        CHECK_EQ((int)img.data[0x00010001], 0x34);
    }

    void TestCrlfAndBlankLines()
    {
        // Files produced on Windows have CRLF endings, and tools often leave a
        // trailing newline; neither may upset the parser.
        const std::string hex =
            "\r\n"
            ":0401000001020304F2\r\n"
            "\r\n"
            ":00000001FF\r\n";

        HexImage img;
        CHECK(ParseHexFile(hex, img));
        CHECK_EQ(img.minAddr, 0x0100u);
        CHECK_EQ(img.maxAddr, 0x0103u);
    }

    void TestLowercaseHexDigits()
    {
        // Some assemblers emit lowercase digits; the Intel HEX spec allows it.
        const std::string hex =
            ":04010000aabbccdd99\n"
            ":00000001ff\n";

        HexImage img;
        CHECK(ParseHexFile(hex, img));
        CHECK_EQ((int)img.data[0x0100], 0xAA);
        CHECK_EQ((int)img.data[0x0103], 0xDD);
    }

    void TestEmptyInputIsRejected()
    {
        HexImage img;
        CHECK(!ParseHexFile("", img));
        CHECK(!ParseHexFile("\n\n\n", img));
    }

    void TestGarbageLinesAreSkipped()
    {
        // A line that is not a record, and one with a truncated body, must be
        // ignored rather than aborting the whole parse.
        const std::string hex =
            "not a record\n"
            ":04\n"
            ":0401000001020304F2\n"
            ":00000001FF\n";

        HexImage img;
        CHECK(ParseHexFile(hex, img));
        CHECK_EQ(img.maxAddr, 0x0103u);
    }

    void TestEofRecordStopsParsing()
    {
        // Anything after the EOF record is not part of the image. Some tools
        // append a comment block there.
        const std::string hex =
            ":0401000001020304F2\n"
            ":00000001FF\n"
            ":04020000FFFFFFFF00\n";

        HexImage img;
        CHECK(ParseHexFile(hex, img));
        CHECK_EQ(img.maxAddr, 0x0103u);
    }

} // namespace

void RunHexFileTests()
{
    TestSimpleImage();
    TestExtendedLinearAddress();
    TestExtendedSegmentAddress();
    TestCrlfAndBlankLines();
    TestLowercaseHexDigits();
    TestEmptyInputIsRejected();
    TestGarbageLinesAreSkipped();
    TestEofRecordStopsParsing();
}
