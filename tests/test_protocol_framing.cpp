// Packet framing and checksum tests.
//
// The framing is what turns a stream of bytes into commands, and getting it
// wrong is silent: a malformed packet is not rejected by the chip, it simply
// produces no answer, which is indistinguishable from a dead board. These tests
// pin the exact bytes.

#include "test.h"

#include "newisp/chip_logic.h"
#include "newisp/protocol_factory.h"
#include "newisp/stc_protocol.h"

#include <memory>

using namespace stc;

// MakeProbeList is part of the chip logic, not the protocol layer.
using newisp::MakeProbeList;

namespace {

    // Build a packet with the base framing and check it byte for byte.
    //
    // The base layout is
    //   46 B9 <dir> <len hi> <len lo> <payload...> <cs hi> <cs lo> 16
    // where len counts the payload plus the six bytes from <dir> through the
    // checksum, and cs is the 16-bit sum of everything from <dir> onward.
    void TestBasePacketLayout()
    {
        auto proto = CreateProtocol("STC32G12K128", false);
        CHECK(proto != nullptr);
        if (!proto) return;

        // The 0x03 erase command with no 5A A5 marker.
        std::vector<uint8_t> payload = { 0x03, 0x00 };
        auto pkt = proto->BuildPacket(payload);

        // Two header bytes + direction + two length bytes + payload + two
        // checksum bytes + terminator = 10.
        // len = payload (2) + 6 = 8 -> 0x0008
        CHECK_EQ(pkt.size(), (size_t)10);
        CHECK_EQ((int)pkt[0], 0x46);
        CHECK_EQ((int)pkt[1], 0xB9);
        CHECK_EQ((int)pkt[2], 0x6A);   // direction: host -> chip
        CHECK_EQ((int)pkt[3], 0x00);
        CHECK_EQ((int)pkt[4], 0x08);
        CHECK_EQ((int)pkt[5], 0x03);
        CHECK_EQ((int)pkt[6], 0x00);

        // Checksum over 6A 00 08 03 00 = 0x0075
        uint16_t cs = (uint16_t)(0x6A + 0x00 + 0x08 + 0x03 + 0x00);
        CHECK_EQ((int)pkt[7], (cs >> 8) & 0xFF);
        CHECK_EQ((int)pkt[8], cs & 0xFF);
        CHECK_EQ((int)pkt[9], 0x16);
    }

    void TestEpilogueAppendsTrailer()
    {
        auto proto = CreateProtocol("STC32G12K128", false);
        if (!proto) return;

        auto plain = proto->BuildPacket({ 0x01 });
        auto padded = proto->BuildPacket({ 0x01 }, 0x6A, 3);

        CHECK_EQ(padded.size(), plain.size() + 3);
        // The trailer is 0x66 repeated; the ISP uses it as a drain marker.
        CHECK_EQ((int)padded[padded.size() - 1], 0x66);
        CHECK_EQ((int)padded[padded.size() - 2], 0x66);
        CHECK_EQ((int)padded[padded.size() - 3], 0x66);
    }

    // The parser must accept a packet the chip would send, for every family.
    //
    // Note the direction byte: BuildPacket defaults to 0x6A (host -> chip),
    // while ParsePacket only accepts 0x68 (chip -> host). That asymmetry is
    // deliberate -- the two directions genuinely use different length and
    // checksum conventions in the STC89 family -- so a naive "build then parse
    // what I built" round trip is wrong for those protocols. The reply is built
    // with the chip's direction instead.
    void TestParseOfChipRepliesForEveryProtocol()
    {
        for (bool useHid : { false, true }) {
            auto list = MakeProbeList(useHid);
            for (auto& proto : list) {
                std::vector<uint8_t> payload = { 0x50, 0x01, 0x02, 0x03, 0x04 };
                auto packet = proto->ProbeBuild(payload, 0x68);

                std::vector<uint8_t> parsed;
                size_t consumed = 0;
                bool ok = proto->ProbeParse(packet, parsed, consumed);

                if (!ok) {
                    ::newisp_test::Report(__FILE__, __LINE__,
                        "parse failed for protocol " +
                        proto->Params().name + (useHid ? " (hid)" : " (uart)"));
                    continue;
                }
                CHECK_EQ(consumed, packet.size());
                CHECK_EQ(parsed.size(), payload.size());
                for (size_t i = 0; i < payload.size() && i < parsed.size(); ++i) {
                    CHECK_EQ((int)parsed[i], (int)payload[i]);
                }
            }
        }
    }

    // The host-direction build must produce the documented header, and the
    // legacy STC89 framing must differ from the base layout.
    //
    // Note which class is which: CreateProtocol() maps every STC89/90 name to
    // ProtocolStc89A (the newer 0x50-status variant, which shares the base
    // framing). The *legacy* ProtocolStc89 is only reachable from the probe
    // list. The two really do frame differently, so the test names the class.
    //
    // With a 2-byte payload:
    //   base  46 B9 6A 00 08 <2 bytes> <cs hi> <cs lo> 16   = 10 bytes
    //   stc89 46 B9 6A 00 07 <2 bytes> <cs>            16   =  9 bytes
    void TestHostDirectionFramingPerFamily()
    {
        auto base = CreateProtocol("STC32G12K128", false);
        CHECK(base != nullptr);
        if (!base) return;

        ProtocolStc89 legacy;

        std::vector<uint8_t> payload = { 0x03, 0x00 };

        auto basePkt = base->BuildPacket(payload);
        CHECK_EQ((int)basePkt[2], 0x6A);
        CHECK_EQ((int)basePkt[4], (int)(payload.size() + 6));
        CHECK_EQ(basePkt.size(), (size_t)10);
        CHECK_EQ((int)basePkt[basePkt.size() - 1], 0x16);

        auto stc89Pkt = legacy.BuildPacket(payload);
        CHECK_EQ((int)stc89Pkt[2], 0x6A);
        CHECK_EQ((int)stc89Pkt[4], (int)(payload.size() + 5));
        CHECK_EQ(stc89Pkt.size(), (size_t)9);
        CHECK_EQ((int)stc89Pkt[stc89Pkt.size() - 1], 0x16);

        // The legacy checksum is a single byte, which is the whole difference.
        CHECK_EQ(stc89Pkt.size(), basePkt.size() - 1);
    }

    // The 89A variant that the factory does return shares the base framing.
    void TestStc89aUsesBaseFraming()
    {
        auto proto = CreateProtocol("STC89C516RD", false);
        CHECK(proto != nullptr);
        if (!proto) return;

        ProtocolParams params = proto->Params();
        CHECK_STREQ(params.name, "stc89a");

        std::vector<uint8_t> payload = { 0x03, 0x00 };
        auto pkt = proto->BuildPacket(payload);
        CHECK_EQ((int)pkt[4], (int)(payload.size() + 6));
        CHECK_EQ(pkt.size(), (size_t)10);
    }

    void TestParserSkipsLeadingNoise()
    {
        // The wakeup byte 0x7F is spammed before the chip answers, and the
        // reply may be preceded by leftovers from a previous round. The parser
        // has to find the packet in the middle of that.
        auto proto = CreateProtocol("STC32G12K128", false);
        if (!proto) return;

        auto packet = proto->BuildPacket({ 0x03, 0x00 });

        std::vector<uint8_t> stream = { 0x7F, 0x7F, 0x00, 0xFF };
        stream.insert(stream.end(), packet.begin(), packet.end());

        std::vector<uint8_t> parsed;
        size_t consumed = 0;
        CHECK(proto->ProbeParse(stream, parsed, consumed));
        CHECK_EQ(consumed, stream.size());   // consumed through the packet end
        CHECK_EQ(parsed.size(), (size_t)2);
        CHECK_EQ((int)parsed[0], 0x03);
    }

    void TestParserRejectsBadChecksum()
    {
        auto proto = CreateProtocol("STC32G12K128", false);
        if (!proto) return;

        auto packet = proto->BuildPacket({ 0x03, 0x00 });
        packet[7] ^= 0xFF;   // corrupt the checksum

        std::vector<uint8_t> parsed;
        size_t consumed = 0;
        CHECK(!proto->ProbeParse(packet, parsed, consumed));
    }

    void TestParserRejectsLostTrailer()
    {
        auto proto = CreateProtocol("STC32G12K128", false);
        if (!proto) return;

        auto packet = proto->BuildPacket({ 0x03, 0x00 });
        // Replace the 0x16 terminator with something else.
        packet[packet.size() - 1] = 0x00;

        std::vector<uint8_t> parsed;
        size_t consumed = 0;
        CHECK(!proto->ProbeParse(packet, parsed, consumed));
    }

    void TestParserWaitsForCompletePacket()
    {
        // A packet that arrives split across two reads must not parse until the
        // tail is present. This is the case the serial handshake depends on.
        auto proto = CreateProtocol("STC32G12K128", false);
        if (!proto) return;

        auto packet = proto->BuildPacket({ 0x50, 0x01, 0x02 });

        std::vector<uint8_t> parsed;
        size_t consumed = 0;

        std::vector<uint8_t> partial(packet.begin(), packet.end() - 1);
        CHECK(!proto->ProbeParse(partial, parsed, consumed));

        CHECK(proto->ProbeParse(packet, parsed, consumed));
        CHECK_EQ(parsed.size(), (size_t)3);
    }

    void TestStatusParsingRejectsWrongFamily()
    {
        // Every protocol sees every reply during probing, so the status decoder
        // is what actually identifies the chip. A packet that parses as framing
        // but carries another family's signature must be rejected.
        //
        // The 0x50 status packet with magic 0xF743 belongs to the STC8H family,
        // so the STC12 decoder (which expects magic 0xD0..0xE6) must refuse it.
        std::vector<uint8_t> status(32, 0);
        status[0] = 0x50;
        // 24 MHz big endian
        status[1] = 0x01; status[2] = 0x6E; status[3] = 0x36; status[4] = 0x00;
        status[17] = 0x72;
        status[18] = 0x55;
        status[20] = 0xF7;
        status[21] = 0x43;

        ProtocolStc12 stc12;
        McuStatus out12{};
        CHECK(!stc12.ParseStatus(status, out12));

        ProtocolStc32Uart stc32;
        McuStatus out32{};
        CHECK(stc32.ParseStatus(status, out32));
        CHECK_EQ(out32.magic, 0xF743);
        CHECK_EQ(out32.clockHz, 24000000u);
        CHECK_EQ((int)out32.bslVersion, 0x72);
    }

    void TestStatusParsingRejectsImplausibleClock()
    {
        // A clock outside 5 kHz .. 60 MHz means this is not a valid reply, even
        // if the framing and magic look right. Accepting it would make the tool
        // compute nonsense trim values and brick the chip.
        std::vector<uint8_t> status(32, 0);
        status[0] = 0x50;
        status[1] = 0xFF; status[2] = 0xFF; status[3] = 0xFF; status[4] = 0xFF;
        status[17] = 0x72;
        status[20] = 0xF7;
        status[21] = 0x43;

        ProtocolStc32Uart stc32;
        McuStatus out{};
        CHECK(!stc32.ParseStatus(status, out));
    }

} // namespace

void RunProtocolFramingTests()
{
    TestBasePacketLayout();
    TestEpilogueAppendsTrailer();
    TestParseOfChipRepliesForEveryProtocol();
    TestHostDirectionFramingPerFamily();
    TestStc89aUsesBaseFraming();
    TestParserSkipsLeadingNoise();
    TestParserRejectsBadChecksum();
    TestParserRejectsLostTrailer();
    TestParserWaitsForCompletePacket();
    TestStatusParsingRejectsWrongFamily();
    TestStatusParsingRejectsImplausibleClock();
}
