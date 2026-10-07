#pragma once
// STC32 / STC8G / STC8 protocol implementations -- serial (UART) transport.
//
// This is the serial half of the pair. The HID half lives in
// protocol_stc8_hid.h; both define the same class names and the selector
// header protocol_stc8.h picks one at compile time.
//
// Differences from the HID build:
//   * the status packet carries a 0x50 prefix
//   * the calibration handshake runs for real: send calibB, measure the IRC
//     against the host's timing, then program the resulting trim and switch
//     to the requested baud rate
//   * the IRC trim triple therefore comes from that measurement rather than
//     from the table (the table is still the documented fallback)
#include "newisp/stc_protocol.h"
#include "newisp/chip_table.h"

namespace stc {

    // ========================================================================
    //  STC32 / STC8H / AI8051U / 8A8K / 8A2K
    // ========================================================================
    class ProtocolStc32Uart : public IStcProtocol {
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

        // Decode a status packet.
        //
        // The two transports frame the reply differently and the first byte
        // tells them apart:
        //
        //   serial the ISP monitor answers the 0x7F wakeup with a 0x50 status
        //          packet, so p[0] must be 0x50.
        // Decode a status packet.
        //
        // Over serial the ISP monitor answers the 0x7F wakeup with a packet
        // that begins with 0x50, so that prefix is checked here. (The HID build
        // sees a different framing and lives in protocol_stc8_hid.h.)
        bool ParseStatus(const std::vector<uint8_t>& p, McuStatus& out) override {
            if (p.size() < 22 || p[0] != 0x50) return false;

            uint16_t magic = ((uint16_t)p[20] << 8) | p[21];

            out.bslVersion = p[17];
            out.bslStepping = p[18];
            out.magic = magic;

            uint32_t hz = ((uint32_t)p[1] << 24) | ((uint32_t)p[2] << 16) |
                ((uint32_t)p[3] << 8) | (uint32_t)p[4];
            if (hz < 5000u || hz > 60000000u) return false;
            out.clockHz = hz;

            out.raw = p;
            if (p.size() >= 17) {
                out.msr[0] = p[9];
                out.msr[1] = p[10];
                out.msr[2] = p[11];
                out.msr[3] = p[15];
                out.msr[4] = p[16];
            }
            return true;
        }

        // Calibration handshake (serial).
        //
        // Send calibB so the chip measures its IRC against the host's timing,
        // decode the reply, then program the resulting trim (pkt3) and switch
        // to the requested baud rate. This only makes sense once a baud rate
        // exists, which is why the HID build has no equivalent.
        bool Handshake(stcisp::IChannel& uart, std::vector<uint8_t>& rxBuf,
            uint32_t targetBaud, const LogFn& log) override
        {
            uart.Flush();
            rxBuf.clear();

            uint32_t userSpeed = m_status.userTargetFreq;
            if (userSpeed == 0) userSpeed = m_status.clockHz;
            if (userSpeed == 0) userSpeed = 11059200;
            uint32_t targetUserCount = (userSpeed + 1200u) / 2400u;

            std::vector<uint8_t> pkt1 = {
                0x00, 0x08,
                0x00, 0x00, 0xFF, 0x00,
                0x00, 0x10, 0xFF, 0x10,
                0x00, 0x20, 0xFF, 0x20,
                0x00, 0x30, 0xFF, 0x30
            };
            auto pktBytes = BuildPacket(pkt1);
            uart.Write(pktBytes);

            log("[校准] 目标频率 " + std::to_string(userSpeed / 1000) +
                " kHz, target_user_count=" + std::to_string(targetUserCount));
            log("[发送] 0x00 0x08 (" + std::to_string(pktBytes.size()) + " 字节):");
            log("     " + HexDump(pktBytes));

            {
                uint64_t t0 = GetTickCount64();
                bool ok = PulseUntilData(uart, 0xFE, 2000, rxBuf);
                uint64_t dt = GetTickCount64() - t0;
                log("[校准] pulse 0xFE 返回=" + std::to_string(ok ? 1 : 0) +
                    ", 耗时=" + std::to_string(dt) + " ms");
                log("[接收] " + std::to_string(rxBuf.size()) + " 字节: " +
                    (rxBuf.empty() ? std::string("空") : HexDump(rxBuf)));
                if (!ok) { log("[校准] 第一轮无响应"); return false; }
            }

            std::vector<uint8_t> resp1;
            if (!WaitPacket(uart, rxBuf, resp1, 2000)) {
                log("[校准] 第一轮读包失败"); return false;
            }
            log("[接收] resp1 (" + std::to_string(resp1.size()) + "): " + HexDump(resp1));
            if (resp1.empty() || resp1[0] != 0x00) return false;
            uint8_t calibLen = resp1.size() > 1 ? resp1[1] : 0;

            uint8_t trimStart = 0x80;
            uint8_t trimRange = 0x00;
            uint8_t dividerUse = 1;
            bool    found = false;

            for (int div = 1; div <= 5 && !found; ++div) {
                uint32_t target = targetUserCount * (uint32_t)div;
                for (int i = 0; i < (int)(calibLen >> 1); ++i) {
                    size_t off = 2 + 4 * i;
                    if (off + 4 > resp1.size() || off + 4 > pkt1.size()) break;
                    uint16_t ca = ((uint16_t)resp1[off] << 8) | resp1[off + 1];
                    uint16_t cb = ((uint16_t)resp1[off + 2] << 8) | resp1[off + 3];
                    uint8_t  ta = pkt1[off];
                    uint8_t  tb = pkt1[off + 2];
                    uint8_t  tr = pkt1[off + 3];
                    if (ca <= target && cb >= target && cb != ca) {
                        double adjD = (double)(target - ca) * (double)(tb - ta)
                            / (double)(cb - ca) + (double)ta;
                        int adj = (int)(adjD + (adjD >= 0 ? 0.5 : -0.5));
                        if (adj < 6)   adj = 6;
                        if (adj > 250) adj = 250;
                        trimStart = (uint8_t)adj;
                        trimRange = tr;
                        dividerUse = (uint8_t)div;
                        found = true;
                        break;
                    }
                }
            }
            if (!found) { trimStart = 0x80; trimRange = 0x00; dividerUse = 1; }

            std::vector<uint8_t> pkt2;
            pkt2.push_back(0x00); pkt2.push_back(0x0C);
            for (int i = -6; i < 6; ++i) {
                pkt2.push_back((uint8_t)(trimStart + i));
                pkt2.push_back(trimRange);
            }
            auto pkt2Bytes = BuildPacket(pkt2);
            uart.Write(pkt2Bytes);
            log("[发送] 0x00 0x0C: " + HexDump(pkt2Bytes));

            if (!PulseUntilData(uart, 0xFE, 2000, rxBuf)) {
                log("[校准] 第二轮无响应"); return false;
            }

            std::vector<uint8_t> resp2;
            if (!WaitPacket(uart, rxBuf, resp2, 2000)) {
                log("[校准] 第二轮读包失败"); return false;
            }
            log("[接收] resp2: " + HexDump(resp2));
            if (resp2.empty() || resp2[0] != 0x00) return false;

            uint32_t target2 = targetUserCount * (uint32_t)dividerUse;
            int bestIdx = 6, bestDelta = 0x7FFFFFFF;
            uint16_t bestCount = 0;
            for (int i = 0; i < 12; ++i) {
                size_t off = 2 + i * 2;
                if (off + 1 >= resp2.size()) break;
                uint16_t cnt = ((uint16_t)resp2[off] << 8) | resp2[off + 1];
                int d = (int)cnt - (int)target2;
                if (d < 0) d = -d;
                if (d < bestDelta) { bestDelta = d; bestIdx = i; bestCount = cnt; }
            }
            int adjFinal = (int)trimStart + (bestIdx - 6);
            if (adjFinal < 0)   adjFinal = 0;
            if (adjFinal > 255) adjFinal = 255;

            m_trimAdj = (uint8_t)adjFinal;
            m_trimRange = trimRange;
            m_trimDivider = dividerUse;
            m_trimFreq = (uint32_t)bestCount * 2400u / (uint32_t)dividerUse;
            m_trimValid = true;

            double brtD = 65536.0 - 24e6 / ((double)targetBaud * 4.0);
            uint16_t brt = (uint16_t)((int)(brtD + 0.5) & 0xFFFF);
            std::vector<uint8_t> pkt3 = {
                0x01, 0x00, 0x00,
                (uint8_t)(brt >> 8), (uint8_t)(brt & 0xFF),
                trimRange, (uint8_t)adjFinal,
                0x98
            };
            auto pkt3Bytes = BuildPacket(pkt3);
            uart.Write(pkt3Bytes);
            log("[发送] 0x01: " + HexDump(pkt3Bytes));

            std::vector<uint8_t> resp3;
            if (!WaitPacket(uart, rxBuf, resp3, 2000)) {
                log("[校准] 切波特率无响应"); return false;
            }
            log("[接收] resp3: " + HexDump(resp3));
            if (resp3.empty() || resp3[0] != 0x01) return false;

            if (!uart.SetBaudRate(targetBaud)) {
                log("[校准] 主机切波特率失败"); return false;
            }
            log("[校准] 主机已切到 " + std::to_string(targetBaud));

            log("[校准] 成功 trim_adj=" + std::to_string(adjFinal) +
                ", trim_range=0x" + HexW(trimRange) +
                ", trim_div=" + std::to_string(dividerUse) +
                ", trim_freq=" + std::to_string(m_trimFreq) + " Hz");
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

        bool ProgramOptions(stcisp::IChannel& uart, std::vector<uint8_t>& rxBuf,
            const LogFn& log) override
        {
            std::vector<uint8_t> opts(40, 0xFF);
            opts[3] = 0x00;
            opts[6] = 0x00;
            opts[22] = 0x00;

            // Frequency / trim source.
            //
            // The serial ISP monitor answers the calibration handshake, which
            // measures the real IRC frequency and derives the trim triple on
            // the spot; that value is authoritative for every frequency the UI
            // can select. The USB HID ISP interface never reports a measurement,
            // so m_trimValid stays false there and we fall back to the
            // per-frequency constants measured against the vendor tool -- which
            // is also where the overclock bands come from.
            uint32_t freq = m_trimFreq;
            if (freq == 0 || !m_trimValid) {
                const FreqTrim* ft = FindTrim(m_status.userTargetFreq
                    ? m_status.userTargetFreq : m_status.clockHz);
                if (!ft && m_status.clockHz)
                    ft = FindTrim(m_status.clockHz);
                if (!ft) {
                    // Nothing measured and nothing tabulated: keep whatever the
                    // chip reported so the option block is not left at zero.
                    ft = FindTrim(m_status.clockHz);
                }

                if (ft) {
                    if (freq == 0) freq = ft->freq;
                    opts[28] = ft->trim[0];
                    opts[29] = ft->trim[1];
                    opts[30] = ft->trim[2];
                    log("[选项] 使用查表 trim（未校准）: freq=" +
                        std::to_string(ft->freq) + " Hz, trim=" +
                        HexW(ft->trim[0]) + " " + HexW(ft->trim[1]) + " " +
                        HexW(ft->trim[2]));
                }
                else {
                    log("[选项] 警告：无校准值与查表项，trim 保持占位值");
                }
            }
            else {
                opts[28] = m_trimAdj;
                opts[29] = m_trimRange;
                opts[30] = m_trimDivider;
            }

            opts[24] = (uint8_t)((freq >> 24) & 0xFF);
            opts[25] = (uint8_t)((freq >> 16) & 0xFF);
            opts[26] = (uint8_t)((freq >> 8) & 0xFF);
            opts[27] = (uint8_t)(freq & 0xFF);

            opts[32] = m_status.msr[0];
            opts[36] = m_status.msr[1];
            opts[37] = m_status.msr[2];
            opts[38] = m_status.msr[3];

            if (m_eepromBytes > 0) {
                uint32_t totalFlash = m_chipTotalBytes;
                if (totalFlash < 1024) totalFlash = 64 * 1024;

                if (m_eepromBytes >= totalFlash) {
                    opts[39] = m_status.msr[4];
                }
                else {
                    uint32_t codeSize = totalFlash - m_eepromBytes;
                    if (codeSize < 512) codeSize = 512;
                    codeSize = (codeSize / 256) * 256;
                    opts[39] = (uint8_t)(codeSize / 256);
                }
            }
            else {
                opts[39] = m_status.msr[4];
            }

            log("[选项] EEPROM=" + std::to_string(m_eepromBytes) +
                ", 总容量=" + std::to_string(m_chipTotalBytes) +
                ", opts[39]=0x" + HexW(opts[39]) +
                " (code=" + std::to_string((uint32_t)opts[39] * 256) + ")");

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

        // Per-frequency trim constants measured against the vendor ISP tool.
        //
        // These are only consulted when the calibration handshake did not
        // produce a measurement -- which is the case on the USB HID
        // ISP interface. trim[0] is the in-band trim, trim[1] the band select
        // and trim[2] the clock divider.
        //
        // The overclock bands (46..50 MHz) exist only here: they are outside
        // the vendor's validated range, so no handshake measurement can be
        // tabulated for them by the chip itself.
        //   freq    target frequency in Hz
        //   vrtrim  per-die reference-voltage trim (0x00 = keep the value the
        //           option block already carries)
        //   trim    { in-band trim, band select, divider }
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

        uint32_t m_trimFreq = 0;
        uint8_t  m_trimAdj = 0;
        uint8_t  m_trimRange = 0;
        uint8_t  m_trimDivider = 2;
        // Set by a successful calibration handshake. When false, ProgramOptions
        // falls back to the measured table above. The HID ISP interface never sets
        // this because it reports no frequency measurement.
        bool     m_trimValid = false;
        std::vector<uint8_t> m_uid;
    };
    // ========================================================================
    //  STC8G：双频段校准
    // ========================================================================
    class ProtocolStc8GUart : public ProtocolStc32Uart {
    public:
        ProtocolParams Params() const override {
            ProtocolParams p;
            p.blockSize = 64;
            p.iapWait = 0x98;
            p.need5AA5 = (m_status.bslVersion >= 0x72);
            p.name = "stc8g";
            return p;
        }

        bool ParseStatus(const std::vector<uint8_t>& p, McuStatus& out) override {
            if (p.size() < 22 || p[0] != 0x50) return false;

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
            if (p.size() >= 17) {
                out.msr[0] = p[9];
                out.msr[1] = p[10];
                out.msr[2] = p[11];
                out.msr[3] = p[15];
                out.msr[4] = p[16];
            }
            return true;
        }

        bool Handshake(stcisp::IChannel& uart, std::vector<uint8_t>& rxBuf,
            uint32_t targetBaud, const LogFn& log) override
        {
            uart.Flush();
            rxBuf.clear();

            uint32_t userSpeed = m_status.userTargetFreq;
            if (userSpeed == 0) userSpeed = m_status.clockHz;
            if (userSpeed == 0) userSpeed = 11059200;

            auto read_u16 = [](const std::vector<uint8_t>& v, size_t off) -> uint16_t {
                return ((uint16_t)v[off] << 8) | v[off + 1];
                };

            std::vector<uint8_t> pkt1 = {
                0x00, 0x05,
                0x02, 0x00, 0x80, 0x00,
                0x00, 0x80, 0x80, 0x80,
                0xFD, 0x00
            };
            auto pkt1Bytes = BuildPacket(pkt1);
            uart.Write(pkt1Bytes);

            log("[校准] STC8G 目标 " + std::to_string(userSpeed / 1000) +
                " kHz，第一轮读频段边界");
            log("[发送] 0x00 0x05: " + HexDump(pkt1Bytes));

            if (!PulseUntilData(uart, 0x66, 2000, rxBuf)) {
                log("[校准] 第一轮无响应"); return false;
            }

            std::vector<uint8_t> resp1;
            if (!WaitPacket(uart, rxBuf, resp1, 2000)) {
                log("[校准] 第一轮读包失败"); return false;
            }
            log("[接收] resp1: " + HexDump(resp1));
            if (resp1.size() < 12 || resp1[0] != 0x00) {
                log("[校准] 第一轮响应异常"); return false;
            }

            uint32_t freq1 = (uint32_t)read_u16(resp1, 2) * 2400u;
            uint32_t freq2 = (uint32_t)read_u16(resp1, 4) * 2400u;
            uint32_t freq3 = (uint32_t)read_u16(resp1, 6) * 2400u;
            uint32_t freq4 = (uint32_t)read_u16(resp1, 8) * 2400u;
            uint32_t freq5 = (uint32_t)read_u16(resp1, 10) * 2400u;

            log("[校准] 频段边界: " +
                std::to_string(freq1 / 1000) + " / " +
                std::to_string(freq2 / 1000) + " / " +
                std::to_string(freq3 / 1000) + " / " +
                std::to_string(freq4 / 1000) + " / " +
                std::to_string(freq5 / 1000) + " kHz");

            uint32_t osc_div = 1;
            while ((uint64_t)userSpeed * osc_div < freq1) osc_div++;
            if (osc_div > 255) osc_div = 255;

            int osc_trimx = 0x80;
            int osc_bandx = 0x00;

            if ((uint64_t)userSpeed * osc_div < freq5) {
                if (freq2 != freq1) {
                    double t = (double)((uint64_t)userSpeed * osc_div - freq1) *
                        (0x80 - 0x02) / (double)(freq2 - freq1) + 0x02;
                    osc_trimx = (int)(t + 0.5);
                }
                osc_bandx = 0x00;
                log("[校准] 选择频带 1 (band=0x00)");
            }
            else {
                if (freq4 != freq3) {
                    double t = (double)((uint64_t)userSpeed * osc_div - freq3) *
                        (0x80 - 0x00) / (double)(freq4 - freq3) + 0x02;
                    osc_trimx = (int)(t + 0.5);
                }
                osc_bandx = 0x80;
                log("[校准] 选择频带 2 (band=0x80)");
            }

            if (osc_trimx < 0)   osc_trimx = 0;
            if (osc_trimx > 255) osc_trimx = 255;

            log("[校准] osc_div=" + std::to_string(osc_div) +
                ", osc_trimx=" + std::to_string(osc_trimx) +
                ", osc_bandx=0x" + HexW((uint8_t)osc_bandx));

            const int COUNT = 24;
            std::vector<uint8_t> pkt2 = { 0x00, (uint8_t)COUNT };
            for (int x = 0; x < COUNT; ++x) {
                int trim = osc_trimx - 2 + (x >> 2);
                int band = osc_bandx + (x & 0x03);
                if (trim < 0)   trim = 0;
                if (trim > 255) trim = 255;
                if (band < 0)   band = 0;
                if (band > 255) band = 255;
                pkt2.push_back((uint8_t)trim);
                pkt2.push_back((uint8_t)band);
            }
            auto pkt2Bytes = BuildPacket(pkt2);
            uart.Write(pkt2Bytes);
            log("[发送] 0x00 0x18 (24 组): " + HexDump(pkt2Bytes));

            if (!PulseUntilData(uart, 0x66, 2000, rxBuf)) {
                log("[校准] 第二轮无响应"); return false;
            }

            std::vector<uint8_t> resp2;
            if (!WaitPacket(uart, rxBuf, resp2, 2000)) {
                log("[校准] 第二轮读包失败"); return false;
            }
            log("[接收] resp2: " + HexDump(resp2));
            if (resp2.size() < (size_t)(2 + COUNT * 2) || resp2[0] != 0x00) {
                log("[校准] 第二轮响应长度不足"); return false;
            }

            uint64_t target_scaled = (uint64_t)userSpeed * osc_div;
            int    bestIdx = 0;
            uint64_t bestDelta = 0xFFFFFFFFFFFFFFFFULL;
            uint32_t bestFreq = 0;

            for (int x = 0; x < COUNT; ++x) {
                uint32_t f = (uint32_t)read_u16(resp2, 2 + x * 2) * 2400u;
                uint64_t d = (f > target_scaled) ? (f - target_scaled)
                    : (target_scaled - f);
                if (d < bestDelta) {
                    bestDelta = d;
                    bestIdx = x;
                    bestFreq = f;
                }
            }

            int final_trim_i = osc_trimx - 2 + (bestIdx >> 2);
            int final_band_i = osc_bandx + (bestIdx & 0x03);
            if (final_trim_i < 0)   final_trim_i = 0;
            if (final_trim_i > 255) final_trim_i = 255;
            if (final_band_i < 0)   final_band_i = 0;
            if (final_band_i > 255) final_band_i = 255;

            uint8_t final_trim = (uint8_t)final_trim_i;
            uint8_t final_band = (uint8_t)final_band_i;
            uint32_t actual_freq = bestFreq / osc_div;

            m_trimAdj = final_trim;
            m_trimRange = final_band;
            m_trimDivider = (uint8_t)osc_div;
            m_trimFreq = actual_freq;
            m_trimValid = true;

            double err_pct = 100.0 * ((double)actual_freq - (double)userSpeed)
                / (double)userSpeed;
            char ebuf[128];
            std::snprintf(ebuf, sizeof(ebuf),
                "[校准] 最优 idx=%d, trim=%u, band=0x%02X, "
                "实际 %.4f MHz, 误差 %+.3f%%",
                bestIdx, (unsigned)final_trim, (unsigned)final_band,
                actual_freq / 1e6, err_pct);
            log(ebuf);

            double brtD = 65536.0 - 24e6 / ((double)targetBaud * 4.0);
            uint16_t brt = (uint16_t)((int)(brtD + 0.5) & 0xFFFF);

            std::vector<uint8_t> pkt3 = {
                0x01, 0x00, 0x00,
                (uint8_t)(brt >> 8), (uint8_t)(brt & 0xFF),
                final_band, final_trim,
                0x98
            };
            auto pkt3Bytes = BuildPacket(pkt3);
            uart.Write(pkt3Bytes);
            log("[发送] 0x01: " + HexDump(pkt3Bytes));

            std::vector<uint8_t> resp3;
            if (!WaitPacket(uart, rxBuf, resp3, 2000)) {
                log("[校准] 切波特率无响应"); return false;
            }
            log("[接收] resp3: " + HexDump(resp3));
            if (resp3.empty() || resp3[0] != 0x01) return false;

            if (!uart.SetBaudRate(targetBaud)) {
                log("[校准] 主机切波特率失败"); return false;
            }
            log("[校准] 主机已切到 " + std::to_string(targetBaud));

            log("[校准] STC8G 校准成功");
            return true;
        }
    };

    class ProtocolStc8Uart : public ProtocolStc32Uart {
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
