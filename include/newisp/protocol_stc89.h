#pragma once
#include "newisp/stc_protocol.h"

namespace stc {

    // ========================================================================
    //  STC89：老 89/90 系列
    //  状态包首字节：0x00，magic 0xf0xx ~ 0xf1xx
    // ========================================================================
    class ProtocolStc89 : public IStcProtocol {
    public:
        ProtocolParams Params() const override {
            ProtocolParams p;
            p.blockSize = 128;
            p.iapWait = 0x80;
            p.need5AA5 = false;
            p.name = "stc89";
            return p;
        }

        bool UseEvenParity() const override { return false; }

        std::vector<uint8_t> BuildPacket(const std::vector<uint8_t>& payload,
            uint8_t dir = 0x6A,
            size_t epilogueLen = 0) const override
        {
            std::vector<uint8_t> p;
            p.reserve(payload.size() + 8 + epilogueLen);
            uint16_t len = (uint16_t)(payload.size() + 5);
            p.push_back(0x46); p.push_back(0xB9); p.push_back(dir);
            p.push_back((uint8_t)((len >> 8) & 0xFF));
            p.push_back((uint8_t)(len & 0xFF));
            for (uint8_t b : payload) p.push_back(b);

            uint8_t cs = 0;
            for (size_t i = 2; i < p.size(); ++i) cs += p[i];
            p.push_back(cs);
            p.push_back(0x16);

            for (size_t i = 0; i < epilogueLen; ++i) p.push_back(0x66);
            return p;
        }

        bool ParsePacket(const std::vector<uint8_t>& buf,
            std::vector<uint8_t>& payload,
            size_t& consumed) const override
        {
            for (size_t i = 0; i + 5 < buf.size(); ++i) {
                if (buf[i] != 0x46 || buf[i + 1] != 0xB9) continue;
                if (buf[i + 2] != 0x68) continue;

                uint16_t len = ((uint16_t)buf[i + 3] << 8) | buf[i + 4];
                if (len < 5) continue;
                size_t payloadLen = len - 5;
                size_t end = i + 5 + payloadLen;
                if (end + 1 >= buf.size()) continue;
                if (buf[end + 1] != 0x16) continue;

                uint8_t csIn = buf[end];
                uint8_t csCalc = 0;
                for (size_t j = i + 2; j < end; ++j) csCalc += buf[j];
                if (csIn != csCalc) continue;

                payload.assign(buf.begin() + i + 5, buf.begin() + i + 5 + payloadLen);
                consumed = end + 2;
                return true;
            }
            return false;
        }

        bool ParseStatus(const std::vector<uint8_t>& p, McuStatus& out) override {
            if (p.size() < 22 || p[0] != 0x00) return false;

            uint16_t magic = ((uint16_t)p[20] << 8) | p[21];
            if ((magic >> 8) < 0xf0 || (magic >> 8) > 0xf1) return false;

            out.bslVersion = p[17];
            out.bslStepping = p[18];
            out.magic = magic;

            bool cpu_6t = !(p[19] & 0x01);
            double cpu_t = cpu_6t ? 6.0 : 12.0;
            uint32_t fc = 0;
            for (int i = 0; i < 8; ++i) {
                fc += ((uint32_t)p[1 + 2 * i] << 8) | p[2 + 2 * i];
            }
            fc /= 8;
            uint32_t hz = (uint32_t)(2400.0 * (double)fc * cpu_t / 7.0);
            if (hz < 5000u || hz > 60000000u) return false;
            out.clockHz = hz;

            out.raw = p;
            out.msr[0] = p[19];
            return true;
        }

        bool Handshake(stcisp::IChannel& uart, std::vector<uint8_t>& rxBuf,
            uint32_t targetBaud, const LogFn& log) override
        {
            uart.Flush();
            rxBuf.clear();

            bool cpu_6t = !(m_status.msr[0] & 0x01);
            int sample_rate = cpu_6t ? 16 : 32;
            double brtD = 65536.0 - (double)m_status.clockHz /
                ((double)targetBaud * sample_rate);
            uint16_t brt = (uint16_t)((int)(brtD + 0.5) & 0xFFFF);
            uint8_t brtCsum = (uint8_t)((2 * (256 - (brt & 0xFF))) & 0xFF);

            uint8_t iap = 0x80;
            if (m_status.clockHz < 5E6)  iap = 0x83;
            else if (m_status.clockHz < 10E6) iap = 0x82;
            else if (m_status.clockHz < 20E6) iap = 0x81;

            uint8_t delay = 0xA0;

            std::vector<uint8_t> pkt1 = {
                0x8F,
                (uint8_t)(brt >> 8), (uint8_t)(brt & 0xFF),
                (uint8_t)(0xFF - ((brt >> 8) & 0xFF)), brtCsum, delay, iap
            };
            auto t1 = BuildPacket(pkt1);
            uart.Write(t1);
            log("[发送] 0x8F: " + HexDump(t1));
            std::this_thread::sleep_for(std::chrono::milliseconds(100));

            if (!uart.SetBaudRate(targetBaud)) {
                log("[校准] 切波特率失败"); return false;
            }
            std::vector<uint8_t> r1;
            bool ok1 = WaitPacket(uart, rxBuf, r1, 2000);
            uart.SetBaudRate(2400);
            if (!ok1 || r1.empty() || r1[0] != 0x8F) {
                log("[校准] 0x8F 无响应"); return false;
            }

            std::vector<uint8_t> pkt2 = {
                0x8E,
                (uint8_t)(brt >> 8), (uint8_t)(brt & 0xFF),
                (uint8_t)(0xFF - ((brt >> 8) & 0xFF)), brtCsum, delay
            };
            auto t2 = BuildPacket(pkt2);
            uart.Write(t2);
            log("[发送] 0x8E: " + HexDump(t2));
            std::this_thread::sleep_for(std::chrono::milliseconds(100));

            if (!uart.SetBaudRate(targetBaud)) {
                log("[校准] 切波特率失败"); return false;
            }
            std::vector<uint8_t> r2;
            bool ok2 = WaitPacket(uart, rxBuf, r2, 2000);
            if (!ok2 || r2.empty() || r2[0] != 0x8E) {
                log("[校准] 0x8E 无响应"); return false;
            }

            std::vector<uint8_t> ping = {
                0x80, 0x00, 0x00, 0x36, 0x01,
                (uint8_t)((m_status.magic >> 8) & 0xFF),
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

            log("[校准] STC89 切波特率成功");
            return true;
        }

        bool EraseFlash(stcisp::IChannel& uart, std::vector<uint8_t>& rxBuf,
            size_t eraseSize, size_t /*flashSize*/,
            const LogFn& log) override
        {
            uint8_t blks = (uint8_t)(((eraseSize + 511) / 512) * 2);
            std::vector<uint8_t> pl = {
                0x84, blks, 0x33, 0x33, 0x33, 0x33, 0x33, 0x33
            };
            auto tx = BuildPacket(pl);
            uart.Write(tx);
            log("[发送] 0x84: " + HexDump(tx));
            std::vector<uint8_t> r;
            if (!WaitPacket(uart, rxBuf, r, 5000)) { log("[4/7] 擦除超时"); return false; }
            log("[接收] " + HexDump(r));
            if (r.empty() || r[0] != 0x80) { log("[4/7] 擦除包错误"); return false; }
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
                pl.push_back(0x00); pl.push_back(0x00); pl.push_back(0x00);
                pl.push_back((uint8_t)(addr >> 8));
                pl.push_back((uint8_t)(addr & 0xFF));
                pl.push_back((uint8_t)((BLOCK >> 8) & 0xFF));
                pl.push_back((uint8_t)(BLOCK & 0xFF));
                for (size_t i = 0; i < BLOCK; ++i) {
                    size_t idx = addr + i;
                    pl.push_back(idx < data.size() ? data[idx] : 0x00);
                }
                while (pl.size() < BLOCK + 7) pl.push_back(0x00);

                auto tx = BuildPacket(pl);
                uart.Write(tx);
                if (blk == 0) log("[发送] 块0: " + HexDump(tx));

                std::vector<uint8_t> r;
                if (!WaitPacket(uart, rxBuf, r, 3000)) {
                    log("[5/7] 第 " + std::to_string(blk) + " 块超时"); return false;
                }
                if (blk == 0) log("[接收] 块0: " + HexDump(r));
                if (r.empty() || r[0] != 0x80) {
                    log("[5/7] 第 " + std::to_string(blk) + " 块响应异常"); return false;
                }
                progress((int)(35 + (blk + 1) * 55 / total));
                if ((blk + 1) % 20 == 0 || blk + 1 == total)
                    log("[5/7] " + std::to_string(blk + 1) + "/" +
                        std::to_string(total) + " 块");
            }
            return true;
        }

        bool ProgramOptions(stcisp::IChannel& uart, std::vector<uint8_t>& rxBuf,
            const LogFn& log) override
        {
            std::vector<uint8_t> pl = { 0x8D, m_status.msr[0], 0xFF, 0xFF, 0xFF };
            auto tx = BuildPacket(pl);
            uart.Write(tx);
            log("[发送] 0x8D: " + HexDump(tx));
            std::vector<uint8_t> r;
            if (!WaitPacket(uart, rxBuf, r, 3000)) { log("[6/7] 选项超时"); return false; }
            log("[接收] " + HexDump(r));
            if (r.empty() || r[0] != 0x8D) { log("[6/7] 选项响应异常"); return false; }
            return true;
        }

        void Disconnect(stcisp::IChannel& uart) override {
            uart.Write(BuildPacket({ 0x82 }));
        }
    };

    // ========================================================================
    //  STC89A：新 89/90 系列
    //  状态包首字节：0x50，magic 0xf0xx ~ 0xf1xx
    // ========================================================================
    class ProtocolStc89A : public IStcProtocol {
    public:
        ProtocolParams Params() const override {
            ProtocolParams p;
            p.blockSize = 128;
            p.iapWait = 0x80;
            p.need5AA5 = false;
            p.name = "stc89a";
            return p;
        }

        bool UseEvenParity() const override { return false; }

        std::vector<uint8_t> BuildPacket(const std::vector<uint8_t>& payload,
            uint8_t dir = 0x6A,
            size_t epilogueLen = 0) const override
        {
            std::vector<uint8_t> p;
            p.reserve(payload.size() + 9 + epilogueLen);
            uint16_t len = (uint16_t)(payload.size() + 6);
            p.push_back(0x46); p.push_back(0xB9); p.push_back(dir);
            p.push_back((uint8_t)((len >> 8) & 0xFF));
            p.push_back((uint8_t)(len & 0xFF));
            for (uint8_t b : payload) p.push_back(b);

            uint16_t cs = 0;
            for (size_t i = 2; i < p.size(); ++i) cs += p[i];
            p.push_back((uint8_t)((cs >> 8) & 0xFF));
            p.push_back((uint8_t)(cs & 0xFF));
            p.push_back(0x16);

            for (size_t i = 0; i < epilogueLen; ++i) p.push_back(0x66);
            return p;
        }

        bool ParsePacket(const std::vector<uint8_t>& buf,
            std::vector<uint8_t>& payload,
            size_t& consumed) const override
        {
            for (size_t i = 0; i + 6 < buf.size(); ++i) {
                if (buf[i] != 0x46 || buf[i + 1] != 0xB9) continue;
                if (buf[i + 2] != 0x68) continue;

                uint16_t len = ((uint16_t)buf[i + 3] << 8) | buf[i + 4];
                if (len < 6) continue;
                size_t payloadLen = len - 6;
                size_t end = i + 5 + payloadLen + 2;
                if (end >= buf.size()) continue;
                if (buf[end] != 0x16) continue;

                uint16_t csIn = ((uint16_t)buf[end - 2] << 8) | buf[end - 1];
                uint16_t csCalc = 0;
                for (size_t j = i + 2; j < end - 2; ++j) csCalc += buf[j];
                if (csIn != csCalc) continue;

                payload.assign(buf.begin() + i + 5, buf.begin() + i + 5 + payloadLen);
                consumed = end + 1;
                return true;
            }
            return false;
        }

        bool ParseStatus(const std::vector<uint8_t>& p, McuStatus& out) override {
            if (p.size() < 22 || p[0] != 0x50) return false;

            uint16_t magic = ((uint16_t)p[20] << 8) | p[21];
            if ((magic >> 8) < 0xf0 || (magic >> 8) > 0xf1) return false;

            out.bslVersion = p[17];
            out.bslStepping = p[18];
            out.magic = magic;

            uint16_t fc = ((uint16_t)p[13] << 8) | p[14];
            uint32_t hz = (uint32_t)(12u * (double)fc * 2400u);
            if (hz < 5000u || hz > 60000000u) return false;
            out.clockHz = hz;

            out.raw = p;
            out.msr[0] = p[1];
            return true;
        }

        bool Handshake(stcisp::IChannel& uart, std::vector<uint8_t>& rxBuf,
            uint32_t targetBaud, const LogFn& log) override
        {
            uart.Flush();
            rxBuf.clear();

            double brtD = 65536.0 - (double)m_status.clockHz /
                ((double)targetBaud * 32.0);
            uint16_t brt = (uint16_t)((int)(brtD + 0.5) & 0xFFFF);

            uint8_t iap = 0x80;
            if (m_status.clockHz < 10E6) iap = 0x83;
            else if (m_status.clockHz < 30E6) iap = 0x82;
            else if (m_status.clockHz < 50E6) iap = 0x81;

            std::vector<uint8_t> pl = {
                0x01,
                (uint8_t)((brt >> 8) & 0xFF),
                (uint8_t)(brt & 0xFF),
                iap
            };
            auto t1 = BuildPacket(pl);
            uart.Write(t1);
            log("[发送] 0x01: " + HexDump(t1));
            std::this_thread::sleep_for(std::chrono::milliseconds(200));

            std::vector<uint8_t> r;
            if (!WaitPacket(uart, rxBuf, r, 2000)) {
                log("[校准] 0x01 无响应"); return false;
            }
            if (r.empty() || r[0] != 0x01) {
                log("[校准] 0x01 响应错"); return false;
            }

            if (!uart.SetBaudRate(targetBaud)) {
                log("[校准] 切波特率失败"); return false;
            }

            std::vector<uint8_t> ping = { 0x05, 0x00, 0x00, 0x46, 0xB9 };
            uart.Write(BuildPacket(ping));
            std::vector<uint8_t> rp;
            if (!WaitPacket(uart, rxBuf, rp, 2000)) {
                log("[校准] ping-pong 超时"); return false;
            }
            if (rp.empty() || rp[0] != 0x05) {
                log("[校准] ping-pong 响应错"); return false;
            }

            log("[校准] STC89A 切波特率成功");
            return true;
        }

        bool EraseFlash(stcisp::IChannel& uart, std::vector<uint8_t>& rxBuf,
            size_t /*eraseSize*/, size_t /*flashSize*/,
            const LogFn& log) override
        {
            std::vector<uint8_t> pl = { 0x03, 0x00, 0x00, 0x46, 0xB9 };
            auto tx = BuildPacket(pl);
            uart.Write(tx);
            log("[发送] 0x03: " + HexDump(tx));
            std::vector<uint8_t> r;
            if (!WaitPacket(uart, rxBuf, r, 5000)) { log("[4/7] 擦除超时"); return false; }
            log("[接收] " + HexDump(r));
            if (r.empty() || r[0] != 0x03) { log("[4/7] 擦除包错误"); return false; }
            return true;
        }

        bool ProgramFlash(stcisp::IChannel& uart, std::vector<uint8_t>& rxBuf,
            const std::vector<uint8_t>& data,
            const ProgressFn& progress, const LogFn& log) override
        {
            const size_t BLOCK = Params().blockSize;
            size_t total = (data.size() + BLOCK - 1) / BLOCK;
            for (size_t blk = 0; blk < total; ++blk) {
                std::vector<uint8_t> pl;
                if (blk == 0) {
                    pl = { 0x22, 0x00, 0x00 };
                }
                else {
                    uint16_t a = (uint16_t)(BLOCK * blk);
                    pl = { 0x02,
                           (uint8_t)((a >> 8) & 0xFF),
                           (uint8_t)(a & 0xFF) };
                }
                pl.push_back(0x46); pl.push_back(0xB9);
                for (size_t i = 0; i < BLOCK; ++i) {
                    size_t idx = blk * BLOCK + i;
                    pl.push_back(idx < data.size() ? data[idx] : 0x00);
                }

                auto tx = BuildPacket(pl);
                uart.Write(tx);
                if (blk == 0) log("[发送] 块0: " + HexDump(tx));

                std::vector<uint8_t> r;
                if (!WaitPacket(uart, rxBuf, r, 3000)) {
                    log("[5/7] 第 " + std::to_string(blk) + " 块超时"); return false;
                }
                if (blk == 0) log("[接收] 块0: " + HexDump(r));
                if (r.empty() || r[0] != 0x02) {
                    log("[5/7] 第 " + std::to_string(blk) + " 块响应异常"); return false;
                }
                progress((int)(35 + (blk + 1) * 55 / total));
                if ((blk + 1) % 20 == 0 || blk + 1 == total)
                    log("[5/7] " + std::to_string(blk + 1) + "/" +
                        std::to_string(total) + " 块");
            }
            return true;
        }

        bool ProgramOptions(stcisp::IChannel& uart, std::vector<uint8_t>& rxBuf,
            const LogFn& log) override
        {
            std::vector<uint8_t> pl = { 0x04, 0x00, 0x00, 0x46, 0xB9, m_status.msr[0] };
            auto tx = BuildPacket(pl);
            uart.Write(tx);
            log("[发送] 0x04: " + HexDump(tx));
            std::vector<uint8_t> r;
            if (!WaitPacket(uart, rxBuf, r, 3000)) { log("[6/7] 选项超时"); return false; }
            log("[接收] " + HexDump(r));
            if (r.empty() || r[0] != 0x04) { log("[6/7] 选项响应异常"); return false; }
            return true;
        }

        void Disconnect(stcisp::IChannel& uart) override {
            uart.Write(BuildPacket({ 0xFF }));
        }
    };

} // namespace stc