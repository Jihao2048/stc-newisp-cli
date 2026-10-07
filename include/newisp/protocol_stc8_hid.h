#pragma once
// STC32 / STC8G / STC8 protocol implementations -- USB HID transport.
//
// This is the HID half of the pair. The serial half lives in
// protocol_stc8_uart.h; both define the same class names and the selector
// header protocol_stc8.h picks one at compile time.
//
// Differences from the serial build are confined to how the transport is
// used, never to the wire format of the chip protocol itself:
//   * the status packet has no 0x50 prefix (it starts with a 0x00 family tag)
//   * there is no baud-rate calibration exchange; the USB link is already
//     synchronised by the host controller
//   * the IRC trim triple comes from the measured table rather than from a
//     live calibration handshake
#include "newisp/stc_protocol.h"
#include "newisp/chip_table.h"

namespace stc {

    // ========================================================================
    //  STC32 / STC8H / AI8051U / 8A8K / 8A2K
    // ========================================================================
    class ProtocolStc32Hid : public IStcProtocol {
    public:
        uint32_t m_eepromBytes = 0;
        uint32_t m_chipTotalBytes = 64 * 1024;

        // This family's option block carries a user-selectable EEPROM split.
        bool SupportsEepromSplit() const override { return true; }
        void SetEepromSplit(uint32_t eepromBytes, uint32_t totalFlash) override {
            m_eepromBytes = eepromBytes;
            m_chipTotalBytes = totalFlash;
        }

        ProtocolParams Params() const override {
            ProtocolParams p;
            p.blockSize = 64;
            p.iapWait = 0x98;
            p.need5AA5 = (m_status.bslVersion >= 0x72);
            p.name = "stc32";
            return p;
        }

        // Status reply layout, verified against a live AI8051U34K64 core board
        // (magic 0x78B4, USER-IRC 40 MHz, BSL 5.5U).
        //
        // The bytes passed here are the packet payload: the 0x46 0xB9 header,
        // the direction code, the length and the checksum are already stripped
        // by ParsePacket.
        //
        //   off  0-4  USER-IRC frequency, big endian (0x02625A00 = 40 MHz)
        //   off  9-11 option bytes / MSR[0..2]
        //   off 15    MSR[3]
        //   off 16    MSR[4] (EEPROM sizing)
        //   off 17    BSL version, high nibble = major, low nibble = minor
        //   off 18    BSL stepping, ASCII letter
        //   off 20-21 chip ID (magic), big endian (0x78B4 = AI8051U34K64)
        //
        // Note: offset 0 is the reply code, which varies between bootloader
        // revisions (0x00 for the init reply on this part). Gating on a fixed
        // value such as 0x50 rejects valid replies, so validate the magic and
        // the clock range instead.
        bool ParseStatus(const std::vector<uint8_t>& p, McuStatus& out) override {
            if (p.size() < 22) return false;

            uint16_t magic = ((uint16_t)p[20] << 8) | p[21];

            out.bslVersion = p[17];
            out.bslStepping = p[18];
            out.magic = magic;

            uint32_t hz = ((uint32_t)p[1] << 24) | ((uint32_t)p[2] << 16) |
                ((uint32_t)p[3] << 8) | (uint32_t)p[4];
            if (hz < 5000u || hz > 60000000u) return false;
            out.clockHz = hz;

            out.raw = p;
            out.msr[0] = p[9];
            out.msr[1] = p[10];
            out.msr[2] = p[11];
            out.msr[3] = p[15];
            out.msr[4] = p[16];
            return true;
        }

        // Over the USB HID bootloader there is no UART to calibrate: the USB
        // link is synchronised by the host controller, so the baud-rate probe
        // that the serial build performed here is meaningless and would only
        // time out waiting for a reply that never comes.
        //
        // What still matters is the *target frequency* written into the option
        // bytes later by ProgramOptions(). The user's selection (UI) takes
        // priority; otherwise keep whatever the chip reported, and finally
        // fall back to the frequency currently in effect.
        bool Handshake(stcisp::IChannel& uart, std::vector<uint8_t>& rxBuf,
            uint32_t targetBaud, const LogFn& log) override
        {
            (void)targetBaud;

            uart.Flush();
            rxBuf.clear();

            uint32_t userSpeed = m_status.userTargetFreq;
            if (userSpeed == 0) userSpeed = m_status.clockHz;

            if (userSpeed == 0) {
                log("[校准] 未能确定目标频率，跳过");
                return false;
            }

            m_trimFreq = userSpeed;
            m_trimAdj = 0x80;
            m_trimRange = 0x00;
            m_trimDivider = 1;

            log("[校准] HID 传输无需校准波特率");
            log("[校准] 目标频率 " + std::to_string(userSpeed / 1000) +
                " kHz（写入选项字节）");
            return true;
        }

        bool EraseFlash(stcisp::IChannel& uart, std::vector<uint8_t>& rxBuf,
            size_t /*codeSize*/, size_t /*flashSize*/,
            const LogFn& log) override
        {
            std::vector<uint8_t> pl = { 0x03, 0x00 };
            if (Params().need5AA5) { pl.push_back(0x00); pl.push_back(0x5A); pl.push_back(0xA5); }
            auto tx = BuildPacket(pl);
            uart.Write(tx);
            log("[发送] 0x03: " + HexDump(tx));
            std::vector<uint8_t> r;
            if (!WaitPacket(uart, rxBuf, r, 5000)) { log("[4/7] 擦除超时"); return false; }
            log("[接收] " + HexDump(r));
            if (r.empty() || r[0] != 0x03) { log("[4/7] 擦除包错误"); return false; }

            if (r.size() >= 8) {
                m_uid.assign(r.begin() + 1, r.begin() + 8);
                log("[UID] " + HexDump(m_uid));
            }
            return true;
        }

        bool ProgramFlash(stcisp::IChannel& uart, std::vector<uint8_t>& rxBuf,
            const std::vector<uint8_t>& data,
            const ProgressFn& progress, const LogFn& log) override
        {
            const size_t BLOCK = Params().blockSize;
            size_t total = (data.size() + BLOCK - 1) / BLOCK;
            for (size_t blk = 0; blk < total; ++blk) {
                size_t addr = blk * BLOCK;
                std::vector<uint8_t> pl;
                pl.push_back(blk == 0 ? 0x22 : 0x02);
                pl.push_back((uint8_t)(addr >> 8));
                pl.push_back((uint8_t)(addr & 0xFF));
                if (Params().need5AA5) { pl.push_back(0x5A); pl.push_back(0xA5); }
                for (size_t i = 0; i < BLOCK; ++i) {
                    size_t idx = addr + i;
                    pl.push_back(idx < data.size() ? data[idx] : 0xFF);
                }
                auto tx = BuildPacket(pl);
                uart.Write(tx);
                if (blk == 0) log("[发送] 块0: " + HexDump(tx));

                std::vector<uint8_t> r;
                if (!WaitPacket(uart, rxBuf, r, 3000)) {
                    log("[5/7] 第 " + std::to_string(blk) + " 块超时"); return false;
                }
                if (blk == 0) log("[接收] 块0: " + HexDump(r));
                if (r.size() < 2 || r[0] != 0x02 || r[1] != 0x54) {
                    log("[5/7] 第 " + std::to_string(blk) + " 块响应异常: " + HexDump(r));
                    return false;
                }
                progress((int)(35 + (blk + 1) * 55 / total));
                if ((blk + 1) % 20 == 0 || blk + 1 == total)
                    log("[5/7] " + std::to_string(blk + 1) + "/" +
                        std::to_string(total) + " 块");
            }
            if (Params().need5AA5) {
                auto tx = BuildPacket({ 0x07, 0x00, 0x00, 0x5A, 0xA5 });
                uart.Write(tx);
                log("[发送] 0x07: " + HexDump(tx));
                std::vector<uint8_t> r;
                if (!WaitPacket(uart, rxBuf, r, 2000)) { log("[5/7] 结束包超时"); return false; }
                log("[接收] " + HexDump(r));
            }
            return true;
        }

        // Option-byte layout, matching the reference stc8usb implementation:
        //
        //   [ 0..23] BLOCK1 fixed  (ff ff ff 00 ff ff 00 ff ... 00 ff)
        //   [24..27] target frequency, big endian (Hz)
        //   [28..30] trim triple (trim, range, divider) for that frequency
        //   [31..39] BLOCK2 fixed  (ff ff ff ff 3e bf af f7 fe)
        //
        // The trim triple is a per-frequency constant, not something derived
        // from a serial calibration, so it must come from a lookup table.
        // Writing a placeholder there leaves the IRC untrimmed and the chip
        // will not start reliably.
        inline static const uint8_t BLOCK1[24] = {
            0xff, 0xff, 0xff, 0x00, 0xff, 0xff, 0x00, 0xff,
            0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
            0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00, 0xff
        };
        inline static const uint8_t BLOCK2[9] = {
            0xff, 0xff, 0xff, 0xff, 0x3e, 0xbf, 0xaf, 0xf7, 0xfe
        };

        // Option-byte frequency entry.
        //
        //   freq    target frequency in Hz, written as a 24-bit fixed point
        //           count (freq / 256) at opts[24..26]
        //   vrtrim  written at opts[27]; a per-die reference-voltage trim that
        //           the factory calibrates. It cannot be derived from the
        //           frequency, so the values below mirror what the official
        //           ISP writes for the same band (0x40 low band, 0x58? / 0x40
        //           high band). 0x00 keeps the previous behaviour where the
        //           table has no measured value.
        //   trim    written at opts[28..30]; layout verified from the official
        //           ISP option packets captured in stcga's protocol notes:
        //           (0x5D, log2(4 / clkdiv), clkdiv)
        struct FreqTrim {
            uint32_t freq;
            uint8_t  vrtrim;
            uint8_t  trim[3];
        };

        static const FreqTrim* FindTrim(uint32_t freq) {
            static const FreqTrim kTable[] = {
                {   250000, 0x00, { 0x48, 0x20, 96 } },
                {   500000, 0x00, { 0x48, 0x20, 48 } },
                {  1000000, 0x00, { 0x48, 0x20, 24 } },
                {  2000000, 0x00, { 0x48, 0x20, 12 } },
                {  3000000, 0x00, { 0x48, 0x20,  8 } },
                {  4000000, 0x00, { 0x48, 0x20,  6 } },
                {  5000000, 0x00, { 0x73, 0x30,  8 } },
                {  6000000, 0x00, { 0x48, 0x20,  4 } },
                {  8000000, 0x00, { 0x48, 0x20,  3 } },
                { 10000000, 0x00, { 0x73, 0x30,  4 } },
                { 12000000, 0x00, { 0x48, 0x20,  2 } },
                { 16000000, 0x00, { 0xc6, 0x30,  3 } },
                { 20000000, 0x00, { 0x73, 0x30,  2 } },
                { 24000000, 0x00, { 0x48, 0x20,  1 } },
                { 27000000, 0x00, { 0x7a, 0x20,  1 } },
                { 30000000, 0x00, { 0xb1, 0x20,  1 } },
                { 35000000, 0x00, { 0x44, 0x30,  1 } },
                { 40000000, 0x00, { 0x73, 0x30,  1 } },

                // 45 MHz belongs to the same IRC band as 35/40/48 MHz, so it
                // needs trim[1] = 0x30 (band select) rather than the low-band
                // 0x20/0x02 values. trim[0] is the in-band trim and varies
                // linearly with the IRC frequency: interpolating between
                // 40 MHz (0x73) and 48 MHz (0xC6) gives ~0xA6 for 45 MHz.
                { 45000000, 0x00, { 0xA6, 0x30,  1 } },

                // Overclock range.
                //
                // Measured on an AI8051U34K64: 46 and 47 MHz run correctly,
                // while 48 MHz falls back to a much lower clock, so the IRC
                // physical limit sits just under 48 MHz. The entries above 47
                // are extrapolated so the UI slider has a full range; treat
                // anything above 47 as unreliable.
                //
                // trims follow a quadratic fitted to the five verified points
                // (35/40/45/46/47 MHz), which reproduces all of them to within
                // 0.2 counts: trim0 = 0.0878f^2 + 2.7877f - 137.16
                { 46000000, 0x00, { 0xB1, 0x30,  1 } },   // 实测通过
                { 47000000, 0x00, { 0xBC, 0x30,  1 } },   // 实测通过
                { 48000000, 0x00, { 0xC7, 0x30,  1 } },   // 实测降频
                { 49000000, 0x00, { 0xD2, 0x30,  1 } },   // 外推，未验证
                { 50000000, 0x00, { 0xDE, 0x30,  1 } },   // 外推，未验证

                {  1382400, 0x00, { 0x28, 0x20, 16 } },
                {  2764800, 0x00, { 0x28, 0x20,  8 } },
                {  5529600, 0x00, { 0x28, 0x20,  4 } },
                { 11059200, 0x00, { 0x28, 0x20,  2 } },
                { 18432000, 0x00, { 0x56, 0x30,  2 } },
                { 22118400, 0x00, { 0x28, 0x20,  1 } },
                { 33177600, 0x00, { 0xe9, 0x20,  1 } },
                { 36864000, 0x00, { 0x56, 0x30,  1 } },
                { 44236800, 0x00, { 0x9f, 0x30,  1 } }
            };
            for (const auto& e : kTable) {
                if (e.freq == freq) return &e;
            }
            return nullptr;
        }

        bool ProgramOptions(stcisp::IChannel& uart, std::vector<uint8_t>& rxBuf,
            const LogFn& log) override
        {
            // Fall back to the chip's own frequency when the UI selection has
            // no trim entry, so the IRC is still programmed with a valid value.
            uint32_t freq = m_status.userTargetFreq ? m_status.userTargetFreq
                                                    : m_status.clockHz;
            const FreqTrim* ft = FindTrim(freq);
            if (!ft && m_status.clockHz) {
                freq = m_status.clockHz;
                ft = FindTrim(freq);
            }
            if (!ft) {
                log("[6/7] 没有 " + std::to_string(freq) +
                    " Hz 对应的 trim 参数，无法写入选项字节");
                return false;
            }

            std::vector<uint8_t> opts;
            opts.insert(opts.end(), BLOCK1, BLOCK1 + sizeof(BLOCK1));

            // opts[24..26] = frequency as a 24-bit count in units of 1/256 Hz.
            // opts[27]    = per-die reference-voltage trim (vrtrim).
            {
                uint32_t count = freq / 256u;
                opts.push_back((uint8_t)((count >> 16) & 0xFF));
                opts.push_back((uint8_t)((count >> 8) & 0xFF));
                opts.push_back((uint8_t)(count & 0xFF));
            }
            opts.push_back(ft->vrtrim);
            opts.push_back(ft->trim[0]);
            opts.push_back(ft->trim[1]);
            opts.push_back(ft->trim[2]);
            opts.insert(opts.end(), BLOCK2, BLOCK2 + sizeof(BLOCK2));

            // EEPROM sizing occupies the last byte of BLOCK2 (opts[39]).
            if (m_eepromBytes > 0) {
                uint32_t totalFlash = m_chipTotalBytes;
                if (totalFlash < 1024) totalFlash = 64 * 1024;

                if (m_eepromBytes < totalFlash) {
                    uint32_t codeSize = totalFlash - m_eepromBytes;
                    if (codeSize < 512) codeSize = 512;
                    codeSize = (codeSize / 256) * 256;
                    opts[39] = (uint8_t)(codeSize / 256);
                }
            }

            log("[选项] 频率=" + std::to_string(freq) +
                " Hz, count=0x" + HexW(freq / 256u, 6) +
                ", vrtrim=0x" + HexW(ft->vrtrim) +
                ", trim=" + HexW(ft->trim[0]) + " " + HexW(ft->trim[1]) +
                " " + HexW(ft->trim[2]) +
                ", EEPROM=" + std::to_string(m_eepromBytes) +
                ", opts[39]=0x" + HexW(opts[39]));

            std::vector<uint8_t> pl = { 0x04, 0x00, 0x00 };
            if (Params().need5AA5) { pl.push_back(0x5A); pl.push_back(0xA5); }
            for (const auto& b : opts) pl.push_back(b);

            auto tx = BuildPacket(pl);
            uart.Write(tx);
            log("[发送] 0x04: " + HexDump(tx));

            std::vector<uint8_t> r;
            if (!WaitPacket(uart, rxBuf, r, 3000)) { log("[6/7] 选项超时"); return false; }
            log("[接收] " + HexDump(r));
            if (r.size() < 2 || r[0] != 0x04 || r[1] != 0x54) {
                log("[6/7] 选项响应异常"); return false;
            }
            return true;
        }

        void Disconnect(stcisp::IChannel& uart) override {
            uart.Write(BuildPacket({ 0xFF }));
        }

    protected:
        uint32_t m_trimFreq = 0;
        uint8_t  m_trimAdj = 0;
        uint8_t  m_trimRange = 0;
        uint8_t  m_trimDivider = 2;
        std::vector<uint8_t> m_uid;
    };

    // ========================================================================
    //  STC8G：双频段校准
    // ========================================================================
    class ProtocolStc8GHid : public ProtocolStc32Hid {
    public:
        ProtocolParams Params() const override {
            ProtocolParams p;
            p.blockSize = 64;
            p.iapWait = 0x98;
            p.need5AA5 = (m_status.bslVersion >= 0x72);
            p.name = "stc8g";
            return p;
        }

        // Same reply layout as ProtocolStc32Hid; see the note there for the
        // verified byte offsets. This variant additionally requires the chip
        // ID to belong to the STC8G / AI8G / JX8G families.
        bool ParseStatus(const std::vector<uint8_t>& p, McuStatus& out) override {
            if (p.size() < 22) return false;

            uint16_t magic = ((uint16_t)p[20] << 8) | p[21];

            std::string name = ChipNameFromTable(magic);
            if (name.empty()) return false;

            bool isStc8g = (name.rfind("STC8G", 0) == 0) ||
                (name.rfind("AI8G", 0) == 0) ||
                (name.rfind("JX8G", 0) == 0);
            if (!isStc8g) return false;

            out.bslVersion = p[17];
            out.bslStepping = p[18];
            out.magic = magic;

            uint32_t hz = ((uint32_t)p[1] << 24) | ((uint32_t)p[2] << 16) |
                ((uint32_t)p[3] << 8) | (uint32_t)p[4];
            if (hz < 5000u || hz > 60000000u) return false;
            out.clockHz = hz;

            out.raw = p;
            out.msr[0] = p[9];
            out.msr[1] = p[10];
            out.msr[2] = p[11];
            out.msr[3] = p[15];
            out.msr[4] = p[16];
            return true;
        }

        // Over the USB HID bootloader no UART calibration applies, exactly as
        // for ProtocolStc32Hid. The dual-band trim sweep below is serial-only, so
        // record the target frequency and let ProgramOptions() write it.
        bool Handshake(stcisp::IChannel& uart, std::vector<uint8_t>& rxBuf,
            uint32_t targetBaud, const LogFn& log) override
        {
            (void)targetBaud;

            uart.Flush();
            rxBuf.clear();

            uint32_t userSpeed = m_status.userTargetFreq;
            if (userSpeed == 0) userSpeed = m_status.clockHz;

            if (userSpeed == 0) {
                log("[校准] 未能确定目标频率，跳过");
                return false;
            }

            m_trimFreq = userSpeed;
            m_trimAdj = 0x80;
            m_trimRange = 0x00;
            m_trimDivider = 1;

            log("[校准] STC8G：HID 传输无需校准波特率");
            log("[校准] 目标频率 " + std::to_string(userSpeed / 1000) +
                " kHz（写入选项字节）");
            return true;
        }
    };

    class ProtocolStc8Hid : public ProtocolStc32Hid {
    public:
        ProtocolParams Params() const override {
            ProtocolParams p;
            p.blockSize = 64;
            p.iapWait = GetIapDelay(m_status.clockHz ? m_status.clockHz : 24000000u);
            p.need5AA5 = (m_status.bslVersion >= 0x72);
            p.name = "stc8";
            return p;
        }
    };

} // namespace stc
