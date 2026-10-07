#pragma once
#include "newisp/stc_protocol.h"

namespace stc {

    // ========================================================================
    //  STC12 通用（10/11/12 系列）
    //  状态包首字节：0x50，magic 0xd0xx ~ 0xe6xx
    // ========================================================================
    class ProtocolStc12 : public IStcProtocol {
    public:
        // This generation stores the clock source in its option bytes, so it is
        // the only family that reports one. See IStcProtocol::ClockSourceText.
        std::string ClockSourceText() const override {
            // Generic 12-series layout: clock source is msr[1] bit1
            // (options.py:297); 1 = external crystal, 0 = internal RC.
            return (m_status.msr[1] & 0x02) ? "外部" : "内部";
        }

        ProtocolParams Params() const override {
            ProtocolParams p;
            p.blockSize = 128;
            p.iapWait = GetIapDelay(m_status.clockHz ? m_status.clockHz : 24000000u);
            p.need5AA5 = false;
            p.name = "stc12";
            return p;
        }

        bool ParseStatus(const std::vector<uint8_t>& p, McuStatus& out) override {
            if (p.size() < 29 || p[0] != 0x50) return false;

            uint16_t magic = ((uint16_t)p[20] << 8) | p[21];
            if ((magic >> 8) < 0xd0 || (magic >> 8) > 0xe6) return false;

            out.bslVersion = p[17];
            out.bslStepping = p[18];
            out.magic = magic;

            uint32_t fc = 0;
            for (int i = 0; i < 8; ++i) {
                if (1 + 2 * i + 1 >= p.size()) break;
                fc += ((uint32_t)p[1 + 2 * i] << 8) | p[2 + 2 * i];
            }
            fc /= 8;
            uint32_t hz = (uint32_t)(2400.0 * (double)fc * 12.0 / 7.0);
            if (hz < 5000u || hz > 60000000u) return false;
            out.clockHz = hz;

            // stcgal Stc12BaseProtocol.initialize_options:
            //   status_packet[23:26] + status_packet[27:28]  -> 4 个 MSR 字节
            // 缺失会导致写入全 0xFF 选项包，改动时钟源/看门狗而变砖。
            out.msr[0] = p[23];
            out.msr[1] = p[24];
            out.msr[2] = p[25];
            out.msr[3] = p[27];

            out.raw = p;
            return true;
        }

        bool Handshake(stcisp::IChannel& uart, std::vector<uint8_t>& rxBuf,
            uint32_t targetBaud, const LogFn& log) override
        {
            uart.Flush();
            rxBuf.clear();

            int brt_i = 256 - (int)((m_status.clockHz + targetBaud * 8) / (targetBaud * 16));
            if (brt_i <= 1 || brt_i > 255) {
                log("[校准] 目标波特率无法设置"); return false;
            }
            uint8_t brt = (uint8_t)brt_i;
            uint8_t brtCsum = (uint8_t)((2 * (256 - brt)) & 0xFF);
            uint8_t delay = 0x80;
            uint8_t iap = GetIapDelay(m_status.clockHz);

            {
                std::vector<uint8_t> pl = {
                    0x50, 0x00, 0x00, 0x36, 0x01,
                    (uint8_t)(m_status.magic >> 8),
                    (uint8_t)(m_status.magic & 0xFF)
                };
                auto tx = BuildPacket(pl);
                uart.Write(tx);
                log("[发送] 0x50: " + HexDump(tx));
                std::vector<uint8_t> r;
                if (!WaitPacket(uart, rxBuf, r, 2000) || r.empty() || r[0] != 0x8f) {
                    log("[校准] 握手首包无响应或响应错"); return false;
                }
            }

            {
                std::vector<uint8_t> pl = { 0x8f, 0xc0, brt, 0x3f, brtCsum, delay, iap };
                auto tx = BuildPacket(pl);
                uart.Write(tx);
                log("[发送] 0x8F: " + HexDump(tx));
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                uart.SetBaudRate(targetBaud);
                std::vector<uint8_t> r;
                bool ok = WaitPacket(uart, rxBuf, r, 2000);
                uart.SetBaudRate(2400);
                if (!ok || r.empty() || r[0] != 0x8f) {
                    log("[校准] 0x8F 响应错"); return false;
                }
            }

            {
                std::vector<uint8_t> pl = { 0x8e, 0xc0, brt, 0x3f, brtCsum, delay };
                auto tx = BuildPacket(pl);
                uart.Write(tx);
                log("[发送] 0x8E: " + HexDump(tx));
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                uart.SetBaudRate(targetBaud);
                std::vector<uint8_t> r;
                bool ok = WaitPacket(uart, rxBuf, r, 2000);
                if (!ok || r.empty() || r[0] != 0x84) {
                    log("[校准] 0x8E 响应错"); return false;
                }
            }

            log("[校准] STC12 切波特率成功");
            return true;
        }

        bool EraseFlash(stcisp::IChannel& uart, std::vector<uint8_t>& rxBuf,
            size_t eraseSize, size_t flashSize,
            const LogFn& log) override
        {
            uint8_t blks = (uint8_t)(((eraseSize + 511) / 512) * 2);
            uint8_t size = (uint8_t)(((flashSize + 511) / 512) * 2);
            std::vector<uint8_t> pl = {
                0x84, 0xFF, 0x00, blks, 0x00, 0x00, size,
                0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                0x00, 0x00, 0x00, 0x00, 0x00
            };
            for (int i = 0x80; i > 0x0D; --i) pl.push_back((uint8_t)i);
            uart.Write(BuildPacket(pl));
            std::vector<uint8_t> r;
            if (!WaitPacket(uart, rxBuf, r, 5000)) { log("[4/7] 擦除超时"); return false; }
            if (r.empty() || r[0] != 0x00) { log("[4/7] 擦除包错误"); return false; }
            // stcgal: 部分 BSL 只在擦除包里回 UID
            if (r.size() >= 8) m_uid.assign(r.begin() + 1, r.begin() + 8);
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
                std::vector<uint8_t> pl(3, 0);
                pl.push_back((uint8_t)(addr >> 8));
                pl.push_back((uint8_t)(addr & 0xFF));
                pl.push_back((uint8_t)((BLOCK >> 8) & 0xFF));
                pl.push_back((uint8_t)(BLOCK & 0xFF));
                for (size_t i = 0; i < BLOCK; ++i) {
                    size_t idx = addr + i;
                    pl.push_back(idx < data.size() ? data[idx] : 0x00);
                }
                uart.Write(BuildPacket(pl));
                std::vector<uint8_t> r;
                if (!WaitPacket(uart, rxBuf, r, 3000)) {
                    log("[5/7] 第 " + std::to_string(blk) + " 块超时");
                    return false;
                }
                if (r.empty() || r[0] != 0x00) {
                    log("[5/7] 第 " + std::to_string(blk) + " 块响应异常");
                    return false;
                }
                progress((int)(35 + (blk + 1) * 55 / total));
            }
            std::vector<uint8_t> pf = { 0x69, 0x00, 0x00, 0x36, 0x01,
                (uint8_t)(m_status.magic >> 8), (uint8_t)(m_status.magic & 0xFF) };
            uart.Write(BuildPacket(pf));
            std::vector<uint8_t> rf;
            if (!WaitPacket(uart, rxBuf, rf, 2000)) { log("[5/7] 结束包超时"); return false; }
            return true;
        }

        bool ProgramOptions(stcisp::IChannel& uart, std::vector<uint8_t>& rxBuf,
            const LogFn& log) override
        {
            // 严格移植 stcgal Stc12OptionsMixIn.program_options
            // (protocols.py:1019-1041)。
            //
            // 关键点：必须把状态包里读回的 MSR 原样写回。
            // 若像以前那样填 0xFF，msr[1] bit1 会被置 1（切到外部晶振），
            // 没有外部晶振的板子下次上电即失去时钟源 => 变砖。
            //
            // stcgal 原注释："it's not 100% clear if the index of msr[3] is
            // consistent between devices, so write it to both indices."
            // 因此 msr[3] 同时写在 index 4 和 index 9。
            std::vector<uint8_t> pl = {
                0x8D,
                m_status.msr[0], m_status.msr[1], m_status.msr[2], m_status.msr[3],
                0xFF, 0xFF, 0xFF, 0xFF, m_status.msr[3], 0xFF,
                0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF
            };

            // 依据用户选择的时钟源设置 MSR 的时钟源位。
            // 通用 12 系列：时钟源在 msr[1] bit1（options.py:297），
            // 1 = 外部晶振，0 = 内部 RC。
            //
            // 不做阻拦：用户明确选择了外部晶振就照写。
            // 但如果芯片当前就是外部晶振而要改成内部，同样照写。
            {
                bool wantExt = m_status.wantExternalClock;
                if (wantExt) pl[2] |= 0x02;      // pl[2] == msr[1]
                else        pl[2] &= (uint8_t)~0x02;
                m_status.msr[1] = pl[2];
            }

            uint32_t clk = m_status.clockHz;
            pl.push_back((uint8_t)((clk >> 24) & 0xFF));
            pl.push_back((uint8_t)((clk >> 16) & 0xFF));
            pl.push_back((uint8_t)((clk >> 8) & 0xFF));
            pl.push_back((uint8_t)(clk & 0xFF));

            log("[选项] MSR=" + HexDump({ m_status.msr[0], m_status.msr[1],
                m_status.msr[2], m_status.msr[3] }) +
                "(时钟源=" + std::string((m_status.msr[1] & 0x02) ? "外部" : "内部") +
                ", 看门狗=" + std::string((m_status.msr[2] & 0x20) ? "关" : "开") + ")");

            uart.Write(BuildPacket(pl));
            std::vector<uint8_t> r;
            if (!WaitPacket(uart, rxBuf, r, 3000)) { log("[6/7] 选项超时"); return false; }
            if (r.empty() || r[0] != 0x50) { log("[6/7] 选项响应异常"); return false; }

            // 擦除包里没带 UID 时，UID 在这个响应包里
            if (m_uid.empty() && r.size() >= 25) {
                m_uid.assign(r.begin() + 18, r.begin() + 25);
                log("[UID] " + HexDump(m_uid));
            }

            // stcgal: STC-ISP 对新版 BSL 会额外发一个 0x50，期待 0x10
            if (m_status.bslVersion >= 0x66) {
                uart.Write(BuildPacket({ 0x50 }));
                std::vector<uint8_t> r2;
                if (!WaitPacket(uart, rxBuf, r2, 2000)) {
                    log("[6/7] 选项收尾包超时"); return false;
                }
                if (r2.empty() || r2[0] != 0x10) {
                    log("[6/7] 选项收尾包响应异常"); return false;
                }
            }
            return true;
        }

        void Disconnect(stcisp::IChannel& uart) override {
            uart.Write(BuildPacket({ 0xFF }));
        }

    protected:
        std::vector<uint8_t> m_uid;
    };

    class ProtocolStc12A : public ProtocolStc12 {
    public:
        // The "A" layout keeps the same bit but in msr[0] instead of msr[1]
        // (options.py:366); ProgramOptions writes it to pl[1].
        std::string ClockSourceText() const override {
            return (m_status.msr[0] & 0x02) ? "外部" : "内部";
        }

        ProtocolParams Params() const override {
            ProtocolParams p;
            p.blockSize = 128;
            p.iapWait = GetIapDelay(m_status.clockHz ? m_status.clockHz : 24000000u);
            p.need5AA5 = false;
            p.name = "stc12a";
            return p;
        }

        bool ParseStatus(const std::vector<uint8_t>& p, McuStatus& out) override {
            if (p.size() < 22 || p[0] != 0x50) return false;

            uint16_t magic = ((uint16_t)p[20] << 8) | p[21];
            if ((magic >> 8) < 0xd0 || (magic >> 8) > 0xe6) return false;

            out.bslVersion = p[17];
            out.bslStepping = p[18];
            out.magic = magic;

            uint32_t fc = 0;
            for (int i = 0; i < 8; ++i) {
                if (1 + 2 * i + 1 >= p.size()) break;
                fc += ((uint32_t)p[1 + 2 * i] << 8) | p[2 + 2 * i];
            }
            fc /= 8;
            uint32_t hz = (uint32_t)(2400.0 * (double)fc * 12.0 / 7.0);
            if (hz < 5000u || hz > 60000000u) return false;
            out.clockHz = hz;

            // stcgal Stc12AProtocol.initialize_options:
            //   status_packet[23:26] + status_packet[29:30]
            out.msr[0] = p[23];
            out.msr[1] = p[24];
            out.msr[2] = p[25];
            out.msr[3] = p[29];

            out.raw = p;
            return true;
        }

        bool Handshake(stcisp::IChannel& uart, std::vector<uint8_t>& rxBuf,
            uint32_t targetBaud, const LogFn& log) override
        {
            uart.Flush();
            rxBuf.clear();

            int brt_i = 256 - (int)((m_status.clockHz + targetBaud * 8) / (targetBaud * 16));
            if (brt_i <= 1 || brt_i > 255) {
                log("[校准] 目标波特率无法设置"); return false;
            }
            uint8_t brt = (uint8_t)brt_i;
            uint8_t brtCsum = (uint8_t)((2 * (256 - brt)) & 0xFF);
            uint8_t delay = 0x80;
            uint8_t iap = GetIapDelay(m_status.clockHz);

            {
                std::vector<uint8_t> pl = { 0x8f, 0xc0, brt, 0x3f, brtCsum, delay, iap };
                auto tx = BuildPacket(pl);
                uart.Write(tx);
                log("[发送] 0x8F: " + HexDump(tx));
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                uart.SetBaudRate(targetBaud);
                std::vector<uint8_t> r;
                bool ok = WaitPacket(uart, rxBuf, r, 2000);
                uart.SetBaudRate(2400);
                if (!ok || r.empty() || r[0] != 0x8f) {
                    log("[校准] 0x8F 响应错"); return false;
                }
            }

            {
                std::vector<uint8_t> pl = { 0x8e, 0xc0, brt, 0x3f, brtCsum, delay };
                auto tx = BuildPacket(pl);
                uart.Write(tx);
                log("[发送] 0x8E: " + HexDump(tx));
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                uart.SetBaudRate(targetBaud);
                std::vector<uint8_t> r;
                bool ok = WaitPacket(uart, rxBuf, r, 2000);
                if (!ok || r.empty() || r[0] != 0x8e) {
                    log("[校准] 0x8E 响应错"); return false;
                }
            }

            {
                std::vector<uint8_t> ping = {
                    0x80, 0x00, 0x00, 0x36, 0x01,
                    (uint8_t)(m_status.magic >> 8),
                    (uint8_t)(m_status.magic & 0xFF)
                };
                for (int i = 0; i < 4; ++i) {
                    uart.Write(BuildPacket(ping));
                    std::vector<uint8_t> rp;
                    if (!WaitPacket(uart, rxBuf, rp, 2000)) {
                        log("[校准] ping-pong 超时"); return false;
                    }
                    if (rp.empty() || rp[0] != 0x80) {
                        log("[校准] ping-pong 响应错"); return false;
                    }
                }
            }

            log("[校准] STC12A 切波特率成功");
            return true;
        }

        // stcgal Stc12AOptionsMixIn.program_options (protocols.py:864-891)
        // 12A 的选项包布局与通用 12 系列不同，且响应魔数是 0x80。
        bool ProgramOptions(stcisp::IChannel& uart, std::vector<uint8_t>& rxBuf,
            const LogFn& log) override
        {
            std::vector<uint8_t> pl = {
                0x8D,
                m_status.msr[0], m_status.msr[1], m_status.msr[2], 0xFF,
                m_status.msr[3]
            };

            // 12A 的时钟源在 msr[0] bit1（options.py:171），与通用系列不同。
            // 不做阻拦：按用户选择设置。msr[0] 在包里出现在 index 1 和
            // index 12 两处（stcgal 原样重复写入），两处都要改。
            {
                bool wantExt = m_status.wantExternalClock;
                if (wantExt) pl[1] |= 0x02;
                else         pl[1] &= (uint8_t)~0x02;
                m_status.msr[0] = pl[1];
            }

            uint32_t clk = m_status.clockHz;
            pl.push_back((uint8_t)((clk >> 24) & 0xFF));
            pl.push_back((uint8_t)((clk >> 16) & 0xFF));
            pl.push_back((uint8_t)((clk >> 8) & 0xFF));
            pl.push_back((uint8_t)(clk & 0xFF));
            pl.push_back(m_status.msr[3]);
            for (int i = 0; i < 7; ++i) pl.push_back(0xFF);
            pl.push_back(m_status.msr[0]);
            pl.push_back(m_status.msr[1]);
            pl.push_back(0xFF); pl.push_back(0xFF);
            pl.push_back(0xFF); pl.push_back(0xFF);
            pl.push_back(m_status.msr[2]);
            for (int i = 0; i < 7; ++i) pl.push_back(0xFF);
            pl.push_back((uint8_t)((clk >> 24) & 0xFF));
            pl.push_back((uint8_t)((clk >> 16) & 0xFF));
            pl.push_back((uint8_t)((clk >> 8) & 0xFF));
            pl.push_back((uint8_t)(clk & 0xFF));
            for (int i = 0; i < 3; ++i) pl.push_back(0xFF);

            log("[选项] MSR=" + HexDump({ m_status.msr[0], m_status.msr[1],
                m_status.msr[2], m_status.msr[3] }) +
                "(时钟源=" + std::string((m_status.msr[0] & 0x02) ? "外部" : "内部") + ")");

            uart.Write(BuildPacket(pl));
            std::vector<uint8_t> r;
            if (!WaitPacket(uart, rxBuf, r, 3000)) { log("[6/7] 选项超时"); return false; }
            if (r.empty() || r[0] != 0x80) { log("[6/7] 选项响应异常"); return false; }

            // stcgal: STC-ISP 对新版 BSL 会额外发一个 0x50，期待 0x10
            if (m_status.bslVersion >= 0x66) {
                uart.Write(BuildPacket({ 0x50 }));
                std::vector<uint8_t> r2;
                if (!WaitPacket(uart, rxBuf, r2, 2000)) {
                    log("[6/7] 选项收尾包超时"); return false;
                }
                if (r2.empty() || r2[0] != 0x10) {
                    log("[6/7] 选项收尾包响应异常"); return false;
                }
            }
            return true;
        }
    };

    class ProtocolStc12B : public ProtocolStc12 {
    public:
        ProtocolParams Params() const override {
            ProtocolParams p;
            p.blockSize = 128;
            p.iapWait = 0x80;
            p.need5AA5 = false;
            p.name = "stc12b";
            return p;
        }
    };

} // namespace stc