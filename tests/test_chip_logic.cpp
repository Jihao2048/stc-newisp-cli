// Chip-family predicate and protocol-selection tests.
//
// These rules decide which protocol implementation talks to a chip and which
// limits apply to it, so a mistake here shows up as a chip that cannot be
// programmed at all.

#include "test.h"

#include "newisp/chip_logic.h"

using namespace newisp;

namespace {

    void TestFamilyPredicates()
    {
        CHECK(IsStc12Family("STC12C5A60S2"));
        CHECK(IsStc12Family("STC10F08XE"));
        CHECK(IsStc12Family("IAP11F06"));
        CHECK(!IsStc12Family("STC15F104E"));
        CHECK(!IsStc12Family("STC8H3K60S4"));

        CHECK(IsStc15Family("STC15F104E"));
        CHECK(IsStc15Family("IAP15W4K58S4"));
        CHECK(IsStc15Family("IRC15W4K"));
        CHECK(IsStc15Family("STC15H2K32S4"));

        CHECK(IsStc8gFamily("STC8G1K08"));
        CHECK(IsStc8gFamily("AI8G1K08"));
        CHECK(IsStc8gFamily("JX8G1K08"));
        CHECK(!IsStc8gFamily("STC8H3K60S4"));

        CHECK(IsExternalCrystalFamily("STC89C516RD"));
        CHECK(IsExternalCrystalFamily("STC90C58RD"));
        CHECK(!IsExternalCrystalFamily("STC12C5A60S2"));
    }

    void TestHidOnlyDetection()
    {
        // The predicate still recognises the family so the tool can mention it,
        // but it no longer gates anything.
        CHECK(IsHidOnlyChip("STC8051U34K64"));
        CHECK(IsHidOnlyChip("AI8051U34K64"));
        CHECK(!IsHidOnlyChip("STC8H3K60S4"));
        CHECK(!IsHidOnlyChip("STC32G12K128"));

        // No chip is refused on transport grounds any more.
        //
        // The rule that used to reject an 8051U over serial was inherited from
        // the vendor material and never measured. Measured on an AI8051U34K64
        // with BSL 7.4U, the serial monitor answers the 0x7F wakeup with a
        // 0x50-prefixed status packet and a full erase/program/option cycle
        // succeeds, so the refusal was wrong. The probe decides now.
        CHECK(!IsUnsupportedForTransport("AI8051U34K64", false));
        CHECK(!IsUnsupportedForTransport("AI8051U34K64", true));
        CHECK(!IsUnsupportedForTransport("STC8H3K60S4", false));
        CHECK(!IsUnsupportedForTransport("", false));
    }

    void TestProtoIndexFromChipName()
    {
        CHECK_EQ(ProtoIdxFromChipName("STC89C516RD"), kProtoIdxStc89);
        CHECK_EQ(ProtoIdxFromChipName("STC90C58RD"), kProtoIdxStc89);
        CHECK_EQ(ProtoIdxFromChipName("STC12C5A60S2"), kProtoIdxStc12);
        CHECK_EQ(ProtoIdxFromChipName("STC15F104E"), kProtoIdxStc15);
        CHECK_EQ(ProtoIdxFromChipName("STC8G1K08"), kProtoIdxStc8G);
        CHECK_EQ(ProtoIdxFromChipName("STC8H3K60S4"), kProtoIdxStc32);
        CHECK_EQ(ProtoIdxFromChipName("STC32G12K128"), kProtoIdxStc32);
        CHECK_EQ(ProtoIdxFromChipName("STC8051U34K64"), kProtoIdxStc32);
        CHECK_EQ(ProtoIdxFromChipName("STC8F2K64S4"), kProtoIdxStc8);

        // Unknown and empty names have no family.
        CHECK_EQ(ProtoIdxFromChipName(""), -1);
        CHECK_EQ(ProtoIdxFromChipName("ATMEGA328P"), -1);
    }

    void TestFrequencyCaps()
    {
        // STC8G and STC15 top out at 35 MHz (index 7); everything else can
        // reach the full vendor-validated 45 MHz.
        CHECK_EQ(FreqMaxIdxFor("STC8G1K08"), kFreqMaxIdxStc8G);
        CHECK_EQ(FreqMaxIdxFor("AI8G1K08"), kFreqMaxIdxStc8G);
        CHECK_EQ(FreqMaxIdxFor("STC15F104E"), kFreqMaxIdxStc15);
        CHECK_EQ(FreqMaxIdxFor("STC32G12K128"), kNumFreq - 1);
        CHECK_EQ(kFreqTable[kFreqMaxIdxStc8G], 35000000u);
        CHECK_EQ(kFreqTable[kFreqMaxIdxStc15], 35000000u);
    }

    void TestLegacyBaudAndPrepareRules()
    {
        // STC89/12 have an 8-bit BRT with a fixed divider and cannot reach past
        // 115200, so the UI and the burn path both clamp them.
        CHECK(IsLegacyBaudProto("stc89"));
        CHECK(IsLegacyBaudProto("stc89a"));
        CHECK(IsLegacyBaudProto("stc12"));
        CHECK(IsLegacyBaudProto("stc12a"));
        CHECK(IsLegacyBaudProto("stc12b"));
        CHECK(!IsLegacyBaudProto("stc15"));
        CHECK(!IsLegacyBaudProto("stc32"));

        // The 0x05 prepare step does not exist on those older families.
        CHECK(!NeedsPrepare05("stc89"));
        CHECK(!NeedsPrepare05("stc89a"));
        CHECK(!NeedsPrepare05("stc12"));
        CHECK(!NeedsPrepare05("stc12a"));
        CHECK(!NeedsPrepare05("stc12b"));
        CHECK(NeedsPrepare05("stc15"));
        CHECK(NeedsPrepare05("stc15a"));
        CHECK(NeedsPrepare05("stc32"));
        CHECK(NeedsPrepare05("stc8g"));
    }

    void TestHidReachability()
    {
        // Only the STC32 relatives and the 8G parts implement the USB ISP
        // interface; the older families have no USB at all.
        CHECK(IsHidReachableProto(kProtoIdxStc32));
        CHECK(IsHidReachableProto(kProtoIdxStc8G));
        CHECK(!IsHidReachableProto(kProtoIdxStc89));
        CHECK(!IsHidReachableProto(kProtoIdxStc12));
        CHECK(!IsHidReachableProto(kProtoIdxStc15));
        CHECK(!IsHidReachableProto(kProtoIdxStc8));
    }

    void TestProbeListSizes()
    {
        // Serial probes all ten variants, because any family may answer.
        auto serial = MakeProbeList(false);
        CHECK_EQ(serial.size(), (size_t)10);

        // HID only probes the three implementations the USB parts use.
        auto hid = MakeProbeList(true);
        CHECK_EQ(hid.size(), (size_t)3);
    }

    void TestProtocolFactory()
    {
        // The factory lives in the protocol namespace, while the predicates
        // above are in newisp; name it explicitly here.
        using stc::CreateProtocol;

        // It must return something for every family the tool claims to support,
        // and nullptr only for genuinely unknown names.
        CHECK(CreateProtocol("STC89C516RD", false) != nullptr);
        CHECK(CreateProtocol("STC12C5A60S2", false) != nullptr);
        CHECK(CreateProtocol("STC15F104E", false) != nullptr);
        CHECK(CreateProtocol("STC8G1K08", false) != nullptr);
        CHECK(CreateProtocol("STC8G1K08", true) != nullptr);
        CHECK(CreateProtocol("STC32G12K128", false) != nullptr);
        CHECK(CreateProtocol("STC32G12K128", true) != nullptr);
        CHECK(CreateProtocol("AI8051U34K64", true) != nullptr);
        CHECK(CreateProtocol("ATMEGA328P", false) == nullptr);

        // The 12A/12B variants are selected by name substring, which is the
        // one place the mapping is subtle enough to be worth pinning down.
        CHECK(CreateProtocol("STC12C5A052", false) != nullptr);
        CHECK(CreateProtocol("STC12LE5A60S2", false) != nullptr);
    }

    void TestFlashSizing()
    {
        // The part number encodes the flash size for the 8/32 families.
        CHECK_EQ(GetChipTotalFlash("STC8H3K60S4", 0xF743), 65536u);
        CHECK_EQ(GetChipTotalFlash("STC32G12K128", 0xF8D1), 131072u);
        CHECK_EQ(GetChipTotalFlash("STC8H1K16", 0xF721), 16384u);
        CHECK_EQ(GetChipTotalFlash("STC8G1K08", 0x0000), 12288u);

        // Unknown names fall back to the ID's high byte: 0xfa and above are the
        // 128K parts.
        CHECK_EQ(GetChipTotalFlash("UNKNOWN", 0xFA00), 128u * 1024u);
        CHECK_EQ(GetChipTotalFlash("UNKNOWN", 0xF700), 64u * 1024u);
    }

    void TestDeviceIdExtraction()
    {
        // Both transports use "<friendly name> (<id>)" in their lists.
        CHECK_STREQ(ExtractDeviceId("USB-SERIAL CH340 (COM7)"), "COM7");
        CHECK_STREQ(ExtractDeviceId("STC USB ISP [34BF:1001] (/dev/hidraw0)"),
            "/dev/hidraw0");

        // With no parentheses the text is returned unchanged, so a bare device
        // name pasted from a terminal still works.
        CHECK_STREQ(ExtractDeviceId("/dev/ttyUSB0"), "/dev/ttyUSB0");
        CHECK_STREQ(ExtractDeviceId("COM7"), "COM7");
        // Empty parentheses are not an id.
        CHECK_STREQ(ExtractDeviceId("thing ()"), "thing ()");
    }

} // namespace

void RunChipLogicTests()
{
    TestFamilyPredicates();
    TestHidOnlyDetection();
    TestProtoIndexFromChipName();
    TestFrequencyCaps();
    TestLegacyBaudAndPrepareRules();
    TestHidReachability();
    TestProbeListSizes();
    TestProtocolFactory();
    TestFlashSizing();
    TestDeviceIdExtraction();
}
