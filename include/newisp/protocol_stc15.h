#pragma once
#include "newisp/stc_protocol.h"
#include <utility>
#include <cstdint>
#include <vector>
#include <string>
#include <chrono>
#include <thread>

namespace stc {

    static constexpr uint32_t kClockMinHz = 5000u;
    static constexpr uint32_t kClockMaxHz = 60000000u;

    // ========================================================================
    //  STC15A：老 15 系列
    // ========================================================================
    class ProtocolStc15A : public IStcProtocol {
    public:
        ProtocolParams Params() const override {
            ProtocolParams p;
            p.blockSize = 64;
            p.iapWait = 0x80;
            p.need5AA5 = false;
            p.name = "stc15a";
            return p;
        }

        bool ParseStatus(const std::vector<uint8_t>& p, McuStatus& out) override {
            if (p.size() < 22 || p[0] != 0x50) return false;

            uint16_t magic = ((uint16_t)p[20] << 8) | p[21];
            if ((magic >> 8) < 0xf2 || (magic >> 8) > 0xf5) return false;

            out.bslVersion = p[17];
            out.bslStepping = p[18];
            out.magic = magic;

            uint32_t fc = 0;
            for (int i = 0; i < 4; ++i) {
                if (1 + 2 * i + 1 >= p.size()) break;
                fc += ((uint32_t)p[1 + 2 * i] << 8) | p[2 + 2 * i];
            }
            fc /= 4;
            uint32_t hz = (uint32_t)(2400.0 * (double)fc * 12.0 / 7.0);
            if (hz < kClockMinHz || hz > kClockMaxHz) return false;
            out.clockHz = hz;

            out.raw = p;
            return true;
        }

        bool Handshake(stcisp::IChannel& uart, std::vector<uint8_t>& rxBuf,
            uint32_t targetBaud, const LogFn& log) override
        {
            uart.Flush();
            rxBuf.clear();

            const uint32_t program_speed = 22118400;

            uint32_t fc_sum = 0;
            for (int i = 0; i < 4; ++i) {
                fc_sum += ((uint32_t)m_status.raw[1 + 2 * i] << 8) | m_status.raw[2 + 2 * i];
            }
            double freq_counter = fc_sum / 4.0;

            uint32_t user_speed = m_status.userTargetFreq ? m_status.userTargetFreq : m_status.clockHz;
            if (user_speed == 0) user_speed = 24000000;

            uint32_t user_count = (uint32_t)(freq_counter * (double)user_speed / (double)m_status.clockHz);
            uint32_t program_count = (uint32_t)(freq_counter * (double)program_speed / (double)m_status.clockHz);

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

            std::vector<uint8_t> trim_data;
            for (int i = 0; i < 7; ++i) {
                if (51 + i < (int)m_status.raw.size()) trim_data.push_back(m_status.raw[51 + i]);
                else trim_data.push_back(0xFF);
            }

            auto read_u16 = [](const std::vector<uint8_t>& v, size_t off) -> uint16_t {
                return ((uint16_t)v[off] << 8) | v[off + 1];
                };

            uint16_t program_trim = 0;
            uint16_t target_trim_start = 0;
            {
                std::vector<uint8_t> pkt = { 0x65 };
                for (auto b : trim_data) pkt.push_back(b);
                pkt.push_back(0xff); pkt.push_back(0xff);
                pkt.push_back(0x06); pkt.push_back(0x06);

                auto append_challenge = [&](uint8_t a, uint8_t b) {
                    pkt.push_back(a); pkt.push_back(b);
                    pkt.push_back(0x02); pkt.push_back(0x00);
                    };
                if (user_speed < 7500000u) {
                    append_challenge(0x18, 0x00);
                    append_challenge(0x18, 0x80);
                    append_challenge(0x18, 0x80);
                    append_challenge(0x18, 0xff);
                }
                else if (user_speed < 10000000u) {
                    append_challenge(0x18, 0x80);
                    append_challenge(0x18, 0xff);
                    append_challenge(0x58, 0x00);
                    append_challenge(0x58, 0xff);
                }
                else if (user_speed < 15000000u) {
                    append_challenge(0x58, 0x00);
                    append_challenge(0x58, 0x80);
                    append_challenge(0x58, 0x80);
                    append_challenge(0x58, 0xff);
                }
                else if (user_speed < 21000000u) {
                    append_challenge(0x58, 0x80);
                    append_challenge(0x58, 0xff);
                    append_challenge(0x98, 0x00);
                    append_challenge(0x98, 0x80);
                }
                else if (user_speed < 31000000u) {
                    append_challenge(0x98, 0x00);
                    append_challenge(0x98, 0x80);
                    append_challenge(0x98, 0x80);
                    append_challenge(0x98, 0xff);
                }
                else {
                    append_challenge(0xd8, 0x00);
                    append_challenge(0xd8, 0x80);
                    append_challenge(0xd8, 0x80);
                    append_challenge(0xd8, 0xb4);
                }
                append_challenge(0x98, 0x00);
                append_challenge(0x98, 0x80);

                auto tx = BuildPacket(pkt);
                uart.Write(tx);
                log("[发送] 0x65 粗扫: " + HexDump(tx));

                if (!PulseUntilData(uart, 0x7f, 1000, rxBuf)) {
                    log("[校准] 粗扫无响应"); return false;
                }
                std::vector<uint8_t> r;
                if (!WaitPacket(uart, rxBuf, r, 2000)) {
                    log("[校准] 粗扫读包失败"); return false;
                }
                log("[接收] 0x65 粗扫: " + HexDump(r));
                if (r.size() < 36 || r[0] != 0x65) {
                    log("[校准] 粗扫响应长度或首字节错"); return false;
                }

                uint16_t ta = read_u16(r, 28), ca = read_u16(r, 30);
                uint16_t tb = read_u16(r, 32), cb = read_u16(r, 34);
                if (ca == cb) { log("[校准] 粗扫失败：ca 等于 cb"); return false; }
                double m = (double)(tb - ta) / (double)(cb - ca);
                double n = (double)ta - m * (double)ca;
                int pt = (int)(m * (double)program_count + n + 0.5);
                if (pt < 0 || pt > 65535) { log("[校准] 编程 trim 越界"); return false; }
                program_trim = (uint16_t)pt;

                uint16_t trim_a = read_u16(r, 12), count_a = read_u16(r, 14);
                uint16_t trim_b = read_u16(r, 16), count_b = read_u16(r, 18);
                uint16_t trim_c = read_u16(r, 20), count_c = read_u16(r, 22);
                uint16_t trim_d = read_u16(r, 24), count_d = read_u16(r, 26);
                uint16_t tta, ttb, tca, tcb;
                if (count_c <= user_count && count_d >= user_count) {
                    tta = trim_c; ttb = trim_d;
                    tca = count_c; tcb = count_d;
                }
                else {
                    tta = trim_a; ttb = trim_b;
                    tca = count_a; tcb = count_b;
                }
                if (tca == tcb) { log("[校准] 细扫区间无效"); return false; }
                double m2 = (double)(ttb - tta) / (double)(tcb - tca);
                double n2 = (double)tta - m2 * (double)tca;
                int target_trim = (int)(m2 * (double)user_count + n2 + 0.5);
                int start = target_trim - 5;
                if (start < (int)tta) start = tta;
                if (start > (int)ttb) start = ttb;
                if (start < 0 || start + 11 > 65535) { log("[校准] 细扫起点越界"); return false; }
                target_trim_start = (uint16_t)start;
            }

            uint16_t user_trim = 0;
            {
                std::vector<uint8_t> pkt = { 0x65 };
                for (auto b : trim_data) pkt.push_back(b);
                pkt.push_back(0xff); pkt.push_back(0xff);
                pkt.push_back(0x06); pkt.push_back(0x0B);
                for (int i = 0; i < 11; ++i) {
                    uint16_t v = (uint16_t)(target_trim_start + i);
                    pkt.push_back((uint8_t)(v >> 8));
                    pkt.push_back((uint8_t)(v & 0xFF));
                    pkt.push_back(0x02); pkt.push_back(0x00);
                }
                auto tx = BuildPacket(pkt);
                uart.Write(tx);
                log("[发送] 0x65 细扫: " + HexDump(tx));

                if (!PulseUntilData(uart, 0x7f, 1000, rxBuf)) {
                    log("[校准] 细扫无响应"); return false;
                }
                std::vector<uint8_t> r;
                if (!WaitPacket(uart, rxBuf, r, 2000)) {
                    log("[校准] 细扫读包失败"); return false;
                }
                log("[接收] 0x65 细扫: " + HexDump(r));
                if (r.size() < 56 || r[0] != 0x65) {
                    log("[校准] 细扫响应长度或首字节错"); return false;
                }

                int best_trim = 0; int best_delta = 0x7FFFFFFF;
                for (int i = 0; i < 11; ++i) {
                    uint16_t trim = read_u16(r, 12 + 4 * i);
                    uint16_t cnt = read_u16(r, 14 + 4 * i);
                    int d = (int)cnt - (int)user_count;
                    if (d < 0) d = -d;
                    if (d < best_delta) { best_delta = d; best_trim = trim; }
                }
                user_trim = (uint16_t)best_trim;
            }

            {
                uint8_t iap = GetIapDelay(program_speed);
                std::vector<uint8_t> pl = { 0x8e };
                pl.push_back((uint8_t)(program_trim >> 8));
                pl.push_back((uint8_t)(program_trim & 0xFF));
                pl.push_back((uint8_t)(230400u / targetBaud));
                pl.push_back(0xa1); pl.push_back(0x64); pl.push_back(0xb8);
                pl.push_back(0x00); pl.push_back(iap);
                pl.push_back(0x20); pl.push_back(0xff); pl.push_back(0x00);
                auto tx = BuildPacket(pl);
                uart.Write(tx);
                log("[发送] 0x8E 切波特率: " + HexDump(tx));
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                uart.SetBaudRate(targetBaud);
                std::vector<uint8_t> r;
                if (!WaitPacket(uart, rxBuf, r, 2000) || r.empty() || r[0] != 0x84) {
                    log("[校准] 0x8E 响应错"); return false;
                }
            }

            log("[校准] STC15A 切波特率成功");
            return true;
        }

        bool EraseFlash(stcisp::IChannel& uart, std::vector<uint8_t>& rxBuf,
            size_t eraseSize, size_t flashSize, const LogFn& log) override
        {
            uint8_t blks = (uint8_t)(((eraseSize + 511) / 512) * 2);
            uint8_t size = (uint8_t)(((flashSize + 511) / 512) * 2);
            std::vector<uint8_t> pl = {
                0x84, 0xFF, 0x00, blks, 0x00, 0x00, size,
                0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                0x00, 0x00, 0x00, 0x00, 0x00
            };
            for (int i = 0x80; i > 0x5e; --i) pl.push_back((uint8_t)i);
            auto tx = BuildPacket(pl);
            uart.Write(tx);
            log("[发送] 0x84 擦除: " + HexDump(tx));
            std::vector<uint8_t> r;
            if (!WaitPacket(uart, rxBuf, r, 5000)) { log("[4/7] 擦除超时"); return false; }
            log("[接收] " + HexDump(r));
            if (r.empty() || r[0] != 0x00) { log("[4/7] 擦除响应错"); return false; }
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
                std::vector<uint8_t> pl(3, 0x00);
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
                if (r.empty() || r[0] != 0x00) {
                    log("[5/7] 第 " + std::to_string(blk) + " 块响应异常"); return false;
                }
                progress((int)(35 + (blk + 1) * 55 / total));
                if ((blk + 1) % 20 == 0 || blk + 1 == total)
                    log("[5/7] " + std::to_string(blk + 1) + "/" +
                        std::to_string(total) + " 块");
            }
            std::vector<uint8_t> pf = { 0x69, 0x00, 0x00, 0x36, 0x01,
                (uint8_t)(m_status.magic >> 8), (uint8_t)(m_status.magic & 0xFF) };
            auto tx = BuildPacket(pf);
            uart.Write(tx);
            log("[发送] 0x69 结束: " + HexDump(tx));
            std::vector<uint8_t> rf;
            if (!WaitPacket(uart, rxBuf, rf, 2000)) { log("[5/7] 结束包超时"); return false; }
            log("[接收] " + HexDump(rf));
            if (rf.empty() || rf[0] != 0x8d) { log("[5/7] 结束包响应异常"); return false; }
            return true;
        }

        bool ProgramOptions(stcisp::IChannel& uart, std::vector<uint8_t>& rxBuf,
            const LogFn& log) override
        {
            std::vector<uint8_t> pl = { 0x8D };
            for (int i = 0; i < 13; ++i) {
                if (23 + i < (int)m_status.raw.size()) pl.push_back(m_status.raw[23 + i]);
                else pl.push_back(0xFF);
            }
            for (int i = 0; i < 6; ++i) pl.push_back(0xFF);
            auto tx = BuildPacket(pl);
            uart.Write(tx);
            log("[发送] 0x8D 选项: " + HexDump(tx));
            std::vector<uint8_t> r;
            if (!WaitPacket(uart, rxBuf, r, 3000)) { log("[6/7] 选项超时"); return false; }
            log("[接收] " + HexDump(r));
            if (r.empty() || r[0] != 0x50) { log("[6/7] 选项响应异常"); return false; }
            return true;
        }

        void Disconnect(stcisp::IChannel& uart) override {
            uart.Write(BuildPacket({ 0xFF }));
        }
    };

    // ========================================================================
    //  STC15：新 15 系列
    // ========================================================================
    class ProtocolStc15 : public ProtocolStc15A {
    public:
        ProtocolParams Params() const override {
            ProtocolParams p;
            p.blockSize = 64;
            p.iapWait = 0x80;
            p.need5AA5 = (m_status.bslVersion >= 0x72);
            p.name = "stc15";
            return p;
        }

        bool ParseStatus(const std::vector<uint8_t>& p, McuStatus& out) override {
            if (p.size() < 22 || p[0] != 0x50) return false;

            uint16_t magic = ((uint16_t)p[20] << 8) | p[21];
            if ((magic >> 8) < 0xf2 || (magic >> 8) > 0xf5) return false;

            out.bslVersion = p[17];
            out.bslStepping = p[18];
            out.magic = magic;

            bool external = (p[7] & 0x01) == 0;
            uint32_t hz = 0;
            if (external) {
                uint16_t cnt = 0;
                if (p.size() >= 15) cnt = ((uint16_t)p[13] << 8) | p[14];
                hz = 2400u * cnt;
            }
            else {
                if (p.size() >= 12) {
                    hz = ((uint32_t)p[8] << 24) | ((uint32_t)p[9] << 16) |
                        ((uint32_t)p[10] << 8) | p[11];
                    if (hz == 0xFFFFFFFF) hz = 0;
                }
            }

            if (hz < kClockMinHz || hz > kClockMaxHz) return false;
            out.clockHz = hz;
            out.raw = p;

            if (p.size() >= 8) {
                out.msr[0] = p[5];
                out.msr[1] = p[6];
                out.msr[2] = p[7];
            }
            if (p.size() >= 13) out.msr[3] = p[12];
            if (p.size() >= 38) out.msr[4] = p[37];
            return true;
        }

        bool Handshake(stcisp::IChannel& uart, std::vector<uint8_t>& rxBuf,
            uint32_t targetBaud, const LogFn& log) override
        {
            uart.Flush();
            rxBuf.clear();

            const uint32_t program_speed = 22118400;

            bool external = (m_status.raw.size() > 7) && ((m_status.raw[7] & 0x01) == 0);
            if (external) {
                std::vector<uint8_t> pl = { 0x01 };
                pl.push_back(m_status.raw[4]);
                pl.push_back(0x40);
                int bauds = (int)(65536.0 - (double)m_status.clockHz / (double)targetBaud / 4.0);
                if (bauds < 0 || bauds > 65535) {
                    log("[校准] 外部时钟切波特率越界"); return false;
                }
                pl.push_back((uint8_t)((bauds >> 8) & 0xFF));
                pl.push_back((uint8_t)(bauds & 0xFF));
                uint8_t iap = GetIapDelay(m_status.clockHz);
                pl.push_back(0x00); pl.push_back(0x00); pl.push_back(iap);
                auto tx = BuildPacket(pl);
                uart.Write(tx);
                log("[发送] 0x01 外部时钟切波特率: " + HexDump(tx));
                std::vector<uint8_t> r;
                if (!WaitPacket(uart, rxBuf, r, 2000) || r.empty() || r[0] != 0x01) {
                    log("[校准] 外部时钟切波特率失败"); return false;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                uart.SetBaudRate(targetBaud);
                m_trimValue = { m_status.raw[4], 0x40 };
                m_trimFreq = 24000000;
                log("[校准] STC15 外部时钟切波特率成功");
                return true;
            }

            uint32_t user_speed = m_status.userTargetFreq ? m_status.userTargetFreq : m_status.clockHz;
            if (user_speed == 0) user_speed = 24000000;

            uint32_t target_user_count = (user_speed + 2400 / 4) / (2400 / 2);
            uint32_t target_prog_count = (program_speed + 2400 / 4) / (2400 / 2);

            std::vector<uint8_t> pkt1 = { 0x00, 0x0C };
            pkt1.push_back(0x00); pkt1.push_back(0xc0); pkt1.push_back(0x80); pkt1.push_back(0xc0); pkt1.push_back(0xff); pkt1.push_back(0xc0);
            pkt1.push_back(0x00); pkt1.push_back(0x80); pkt1.push_back(0x80); pkt1.push_back(0x80); pkt1.push_back(0xff); pkt1.push_back(0x80);
            pkt1.push_back(0x00); pkt1.push_back(0x40); pkt1.push_back(0x80); pkt1.push_back(0x40); pkt1.push_back(0xff); pkt1.push_back(0x40);
            pkt1.push_back(0x00); pkt1.push_back(0x00); pkt1.push_back(0x80); pkt1.push_back(0x00); pkt1.push_back(0xc0); pkt1.push_back(0x00);
            uart.Write(BuildPacket(pkt1));
            log("[发送] 0x00 0x0C 第一轮: " + HexDump(pkt1));

            if (!PulseUntilData(uart, 0xfe, 2000, rxBuf)) {
                log("[校准] 第一轮无响应"); return false;
            }
            std::vector<uint8_t> r1;
            if (!WaitPacket(uart, rxBuf, r1, 2000) || r1.size() < 2 || r1[0] != 0x00) {
                log("[校准] 第一轮读包失败"); return false;
            }
            log("[接收] 0x00 第一轮: " + HexDump(r1));

            auto choose_range = [](const std::vector<uint8_t>& resp,
                const std::vector<uint8_t>& chal,
                uint32_t target) -> std::pair<int, uint8_t>
                {
                    if (resp.size() < 2) return { -1, 0 };
                    int calibLen = (int)resp[1];
                    for (int i = 0; i < calibLen - 1; ++i) {
                        size_t ro = 2 + 2 * i;
                        if (ro + 4 > resp.size()) break;
                        size_t co = 2 + 2 * i;
                        if (co + 4 > chal.size()) break;
                        uint16_t count_a = ((uint16_t)resp[ro] << 8) | resp[ro + 1];
                        uint16_t count_b = ((uint16_t)resp[ro + 2] << 8) | resp[ro + 3];
                        uint8_t trim_a = chal[co];
                        uint8_t trim_b = chal[co + 2];
                        uint8_t trim_range = chal[co + 3];
                        if ((count_a <= target && count_b >= target) ||
                            (count_b <= target && count_a >= target)) {
                            if (count_b == count_a) continue;
                            double m = (double)(trim_b - trim_a) / (double)(count_b - count_a);
                            double n = (double)trim_a - m * (double)count_a;
                            int t = (int)(m * (double)target + n + 0.5);
                            if (t < 0 || t > 65535) return { -1, 0 };
                            return { t, trim_range };
                        }
                    }
                    return { -1, 0 };
                };

            auto user_range = choose_range(r1, pkt1, target_user_count);
            auto prog_range = choose_range(r1, pkt1, target_prog_count);
            if (user_range.first < 0 || prog_range.first < 0) {
                log("[校准] 选择粗扫区间失败"); return false;
            }
            uint8_t user_trim_adj = (uint8_t)user_range.first;
            uint8_t user_trim_range = user_range.second;
            uint8_t prog_trim_adj = (uint8_t)prog_range.first;
            uint8_t prog_trim_range = prog_range.second;

            log("[校准] 用户 trim=" + std::to_string(user_trim_adj) +
                " range=0x" + HexW(user_trim_range) +
                "，编程 trim=" + std::to_string(prog_trim_adj) +
                " range=0x" + HexW(prog_trim_range));

            std::vector<uint8_t> pkt2 = { 0x00, 0x0C };
            for (int i = -3; i < 3; ++i) {
                pkt2.push_back((uint8_t)((user_trim_adj + i) & 0xFF));
                pkt2.push_back(user_trim_range);
            }
            for (int i = -3; i < 3; ++i) {
                pkt2.push_back((uint8_t)((prog_trim_adj + i) & 0xFF));
                pkt2.push_back(prog_trim_range);
            }
            uart.Write(BuildPacket(pkt2));
            log("[发送] 0x00 0x0C 第二轮: " + HexDump(pkt2));
            if (!PulseUntilData(uart, 0xfe, 2000, rxBuf)) {
                log("[校准] 第二轮无响应"); return false;
            }
            std::vector<uint8_t> r2;
            if (!WaitPacket(uart, rxBuf, r2, 2000) || r2.size() < 2 || r2[0] != 0x00) {
                log("[校准] 第二轮读包失败"); return false;
            }
            log("[接收] 0x00 第二轮: " + HexDump(r2));

            auto choose_trim = [](const std::vector<uint8_t>& resp,
                const std::vector<uint8_t>& chal,
                uint32_t target) -> std::pair<uint8_t, uint8_t>
                {
                    if (resp.size() < 2) return { 0, 0 };
                    int calibLen = (int)resp[1];
                    int bestIdx = -1;
                    int bestDelta = 0x7FFFFFFF;
                    for (int i = 0; i < calibLen; ++i) {
                        size_t ro = 2 + 2 * i;
                        if (ro + 2 > resp.size()) break;
                        uint16_t cnt = ((uint16_t)resp[ro] << 8) | resp[ro + 1];
                        int d = (int)cnt - (int)target;
                        if (d < 0) d = -d;
                        if (d < bestDelta) { bestDelta = d; bestIdx = i; }
                    }
                    if (bestIdx < 0) return { 0, 0 };
                    size_t co = 2 + 2 * bestIdx;
                    if (co + 2 > chal.size()) return { 0, 0 };
                    return { chal[co], chal[co + 1] };
                };

            auto user_trim_final = choose_trim(r2, pkt2, target_user_count);
            auto prog_trim_final = choose_trim(r2, pkt2, target_prog_count);
            log("[校准] 最终用户 trim=" + std::to_string(user_trim_final.first) +
                " range=0x" + HexW(user_trim_final.second) +
                "，编程 trim=" + std::to_string(prog_trim_final.first) +
                " range=0x" + HexW(prog_trim_final.second));

            m_trimValue = user_trim_final;

            std::vector<uint8_t> pl3 = { 0x01 };
            pl3.push_back(prog_trim_final.first);
            pl3.push_back(prog_trim_final.second);

            if ((m_status.magic >> 8) == 0xf2) {
                uint32_t b1 = (uint32_t)(65536.0 - (double)program_speed / (double)targetBaud);
                uint32_t b2 = (uint32_t)(65536.0 - (double)program_speed / 2.0 * 3.0 / (double)targetBaud);
                pl3.push_back((uint8_t)((b1 >> 8) & 0xFF));
                pl3.push_back((uint8_t)(b1 & 0xFF));
                pl3.push_back((uint8_t)((b2 >> 8) & 0xFF));
                pl3.push_back((uint8_t)(b2 & 0xFF));
            }
            else {
                uint32_t b = (uint32_t)(65536.0 - (double)program_speed / ((double)targetBaud * 4.0));
                pl3.push_back((uint8_t)((b >> 8) & 0xFF));
                pl3.push_back((uint8_t)(b & 0xFF));
                pl3.push_back(user_trim_final.second);
                pl3.push_back(user_trim_final.first);
            }
            uint8_t iap = GetIapDelay(program_speed);
            pl3.push_back(iap);

            uart.Write(BuildPacket(pl3));
            log("[发送] 0x01 切波特率: " + HexDump(pl3));
            std::vector<uint8_t> r3;
            if (!WaitPacket(uart, rxBuf, r3, 2000) || r3.empty() || r3[0] != 0x01) {
                log("[校准] 0x01 响应错"); return false;
            }
            log("[接收] 0x01: " + HexDump(r3));
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            uart.SetBaudRate(targetBaud);

            m_trimFreq = user_speed;

            log("[校准] STC15 切波特率成功");
            return true;
        }

        bool EraseFlash(stcisp::IChannel& uart, std::vector<uint8_t>& rxBuf,
            size_t eraseSize, size_t flashSize, const LogFn& log) override
        {
            std::vector<uint8_t> pl = { 0x03 };
            pl.push_back(eraseSize <= flashSize ? 0x00 : 0x01);
            if (m_status.bslVersion >= 0x72) {
                pl.push_back(0x00); pl.push_back(0x5A); pl.push_back(0xA5);
            }
            auto tx = BuildPacket(pl);
            uart.Write(tx);
            log("[发送] 0x03 擦除: " + HexDump(tx));
            std::vector<uint8_t> r;
            if (!WaitPacket(uart, rxBuf, r, 5000)) { log("[4/7] 擦除超时"); return false; }
            log("[接收] " + HexDump(r));
            if (r.empty() || r[0] != 0x03) { log("[4/7] 擦除响应错"); return false; }
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
                if (m_status.bslVersion >= 0x72) {
                    pl.push_back(0x5A); pl.push_back(0xA5);
                }
                for (size_t i = 0; i < BLOCK; ++i) {
                    size_t idx = addr + i;
                    pl.push_back(idx < data.size() ? data[idx] : 0x00);
                }
                while (pl.size() < BLOCK + 3) pl.push_back(0x00);

                auto tx = BuildPacket(pl);
                uart.Write(tx);
                if (blk == 0) log("[发送] 块0: " + HexDump(tx));

                std::vector<uint8_t> r;
                if (!WaitPacket(uart, rxBuf, r, 3000)) {
                    log("[5/7] 第 " + std::to_string(blk) + " 块超时"); return false;
                }
                if (blk == 0) log("[接收] 块0: " + HexDump(r));
                if (r.size() < 2 || r[0] != 0x02 || r[1] != 0x54) {
                    log("[5/7] 第 " + std::to_string(blk) + " 块响应异常"); return false;
                }
                progress((int)(35 + (blk + 1) * 55 / total));
                if ((blk + 1) % 20 == 0 || blk + 1 == total)
                    log("[5/7] " + std::to_string(blk + 1) + "/" +
                        std::to_string(total) + " 块");
            }
            if (m_status.bslVersion >= 0x72) {
                std::vector<uint8_t> pf = { 0x07, 0x00, 0x00, 0x5A, 0xA5 };
                auto tx = BuildPacket(pf);
                uart.Write(tx);
                log("[发送] 0x07 结束: " + HexDump(tx));
                std::vector<uint8_t> rf;
                if (!WaitPacket(uart, rxBuf, rf, 2000)) { log("[5/7] 结束包超时"); return false; }
                log("[接收] " + HexDump(rf));
                if (rf.size() < 2 || rf[0] != 0x07 || rf[1] != 0x54) {
                    log("[5/7] 结束包响应异常"); return false;
                }
            }
            return true;
        }

        std::vector<uint8_t> BuildOptions() const {
            std::vector<uint8_t> pkt(64, 0xFF);
            pkt[23] = (uint8_t)((m_trimFreq >> 24) & 0xFF);
            pkt[25] = (uint8_t)((m_trimFreq >> 16) & 0xFF);
            pkt[27] = (uint8_t)((m_trimFreq >> 8) & 0xFF);
            pkt[29] = (uint8_t)(m_trimFreq & 0xFF);
            pkt[31] = m_status.msr[3];
            pkt[55] = m_status.msr[4];
            pkt[59] = m_trimValue.first;
            pkt[60] = (uint8_t)(m_trimValue.second + 0x3F);
            pkt[61] = m_status.msr[0];
            pkt[62] = m_status.msr[1];
            pkt[63] = m_status.msr[2];
            return pkt;
        }

        bool ProgramOptions(stcisp::IChannel& uart, std::vector<uint8_t>& rxBuf,
            const LogFn& log) override
        {
            std::vector<uint8_t> pl = { 0x04, 0x00, 0x00 };
            if (m_status.bslVersion >= 0x72) {
                pl.push_back(0x5A); pl.push_back(0xA5);
            }
            auto opts = BuildOptions();
            for (auto b : opts) pl.push_back(b);
            auto tx = BuildPacket(pl);
            uart.Write(tx);
            log("[发送] 0x04 选项: " + HexDump(tx));
            std::vector<uint8_t> r;
            if (!WaitPacket(uart, rxBuf, r, 3000)) { log("[6/7] 选项超时"); return false; }
            log("[接收] " + HexDump(r));
            if (r.size() < 2 || r[0] != 0x04 || r[1] != 0x54) {
                log("[6/7] 选项响应异常"); return false;
            }
            return true;
        }

    private:
        uint32_t m_trimFreq = 24000000;
        std::pair<uint8_t, uint8_t> m_trimValue = { 0x80, 0x40 };
    };

} // namespace stc