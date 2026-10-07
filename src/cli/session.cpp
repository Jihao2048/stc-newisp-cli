// Session layer implementation.
//
// The sequences here are ported from the WinUI build's OpenSession,
// DetectWorker and BurnWorker. The wire protocol is untouched; what changed is
// that UI calls became callbacks, and the process-global stop flag became an
// atomic reference so the caller keeps ownership.

#include "newisp/session.h"

#include "newisp/chip_table.h"
#include "newisp/hex_file.h"
#include "newisp/hid_channel.h"
#include "newisp/serial_channel.h"
#include "newisp/text.h"

#include <cstdio>

using namespace stc;

namespace newisp {

    namespace {

        // ---------------------------------------------------------------
        //  Probing
        // ---------------------------------------------------------------

        // Try every protocol's packet parser against the receive buffer.
        //
        // A reply is only accepted when the framing parses *and* the status
        // decoder recognises it: the packet header alone is far too weak a
        // signal, since several families share 0x46 0xB9 framing and the
        // checksum is a simple sum.
        bool TryParseAll(std::vector<std::unique_ptr<IStcProtocol>>& protoList,
            std::vector<uint8_t>& rxBuf,
            SessionResult& res,
            const LogFn& log,
            const char* tag)
        {
            if (rxBuf.empty()) return false;

            for (auto& proto : protoList) {
                std::vector<uint8_t> pl;
                size_t consumed = 0;
                if (!proto->ProbeParse(rxBuf, pl, consumed)) continue;

                McuStatus trySt{};
                if (!proto->ParseStatus(pl, trySt)) continue;

                log(std::string("[接收] (") + tag + ") payload " +
                    std::to_string(pl.size()) + ": " + HexDump(pl));

                res.got = true;
                res.st = trySt;
                res.protoName = proto->Params().name;
                rxBuf.erase(rxBuf.begin(),
                    rxBuf.begin() + (ptrdiff_t)consumed);
                log(std::string("[") + tag + "] 识别为 " + res.protoName);
                return true;
            }

            // Bound the buffer: a chatty or miswired line must not grow this
            // without limit across 500 handshake rounds.
            if (rxBuf.size() > 512) {
                rxBuf.erase(rxBuf.begin(),
                    rxBuf.begin() + (ptrdiff_t)(rxBuf.size() - 256));
            }
            return false;
        }

    } // namespace

    // =======================================================================
    //  Bring-up
    // =======================================================================

    SessionResult ConnectSerial(stcisp::IChannel& ch,
        const std::string& deviceId,
        std::vector<uint8_t>& rxBuf,
        std::vector<std::unique_ptr<IStcProtocol>>& protoList,
        const LogFn& log,
        std::atomic<bool>& stopReq,
        const char* tag)
    {
        SessionResult res;
        const int MAX_ROUNDS = 500;
        const int PULSE_PER_ROUND = 200;
        const int RETRY_WAIT_MS = 500;

        if (protoList.empty()) return res;
        bool resetDone = false;

        for (int round = 1; round <= MAX_ROUNDS && !res.got; ++round) {
            if (stopReq.load()) break;

            if (!ch.Open(deviceId, 2400, protoList[0]->UseEvenParity())) {
                log(std::string("[") + tag +
                    "] 打开串口失败，请检查端口是否被占用或选择正确");
                res.openFailed = true;
                return res;
            }

            if (!resetDone) {
                resetDone = true;
                log(std::string("[") + tag + "] 自动复位脉冲 DTR 250ms");
                ch.PulseReset(0, 250, 30);
            }

            rxBuf.clear();
            ch.Flush();
            log(std::string("[") + tag + "] 第 " + std::to_string(round) +
                " 轮握手...");

            std::vector<uint8_t> wake = { 0x7F };

            for (int p = 0; p < PULSE_PER_ROUND && !res.got; ++p) {
                if (stopReq.load()) break;

                ch.Write(wake);

                bool gotAny = false;
                {
                    uint64_t t0 = NowMs();
                    while (NowMs() - t0 < 30) {
                        if (stopReq.load()) break;
                        auto c = ch.Read(1024, 0);
                        if (!c.empty()) {
                            rxBuf.insert(rxBuf.end(), c.begin(), c.end());
                            gotAny = true;
                            break;
                        }
                        SleepMs(2);
                    }
                }

                if (gotAny) {
                    // Something answered. Keep reading until the line goes
                    // quiet: a status packet can arrive in several USB chunks,
                    // and parsing mid-packet would reject it.
                    uint64_t lastDataMs = NowMs();
                    uint64_t startMs = NowMs();
                    const uint64_t MAX_MS = 400;
                    const uint64_t QUIET_MS = 60;

                    while ((NowMs() - startMs) < MAX_MS) {
                        if (stopReq.load()) break;

                        if (TryParseAll(protoList, rxBuf, res, log, tag)) break;
                        if (res.got) break;

                        auto c2 = ch.Read(1024, 0);
                        if (!c2.empty()) {
                            rxBuf.insert(rxBuf.end(), c2.begin(), c2.end());
                            lastDataMs = NowMs();
                        }
                        else {
                            if ((NowMs() - lastDataMs) >= QUIET_MS) break;
                            SleepMs(3);
                        }
                    }
                }

                if (res.got) break;

                if ((p + 1) % 20 == 0) {
                    log(std::string("[") + tag + "] 第 " +
                        std::to_string(round) + " 轮: 已 pulse " +
                        std::to_string(p + 1) + "/" +
                        std::to_string(PULSE_PER_ROUND));
                }
            }

            if (res.got) break;
            ch.Close();
            log(std::string("[") + tag + "] 第 " + std::to_string(round) +
                " 轮无响应，重试...");
            for (int ms = 0; ms < RETRY_WAIT_MS; ms += 50) {
                if (stopReq.load()) break;
                SleepMs(50);
            }
        }

        return res;
    }

    SessionResult ConnectHid(stcisp::IChannel& ch,
        const std::string& deviceId,
        std::vector<uint8_t>& rxBuf,
        std::vector<std::unique_ptr<IStcProtocol>>& protoList,
        const LogFn& log,
        std::atomic<bool>& stopReq,
        const char* tag)
    {
        SessionResult res;

        if (protoList.empty()) return res;

        if (!ch.Open(deviceId, 0, protoList[0]->UseEvenParity())) {
            log(std::string("[") + tag +
                "] 打开 HID 设备失败，请检查设备是否被占用");
            res.openFailed = true;
            return res;
        }

        rxBuf.clear();
        ch.Flush();

        // Probe every protocol with its own init payload; the ISP monitor
        // answers only for the family it belongs to.
        for (auto& proto : protoList) {
            if (stopReq.load() || res.got) break;

            std::vector<uint8_t> init = proto->ProbeBuild({ 0x00 });
            if (!ch.Write(init)) {
                // A failed write is silent otherwise, and it is the one thing
                // that makes the whole probe look like an absent chip. Say so.
                log(std::string("[") + tag + "] 写入探测包失败 (" +
                    proto->Params().name + ")");
                continue;
            }

            uint64_t t0 = NowMs();
            while (!res.got && (NowMs() - t0) < 1000) {
                if (stopReq.load()) break;
                auto c = ch.Read(512, 50);
                if (!c.empty()) rxBuf.insert(rxBuf.end(), c.begin(), c.end());
                if (TryParseAll(protoList, rxBuf, res, log, tag)) break;
            }
            if (!res.got) {
                log(std::string("[") + tag + "] " + proto->Params().name +
                    " 无应答（已收 " + std::to_string(rxBuf.size()) + " 字节）");
            }
            if (res.got) break;
        }

        if (!res.got) ch.Close();
        return res;
    }

    SessionResult OpenSession(stcisp::IChannel& ch,
        const std::string& deviceId,
        std::vector<uint8_t>& rxBuf,
        std::vector<std::unique_ptr<IStcProtocol>>& protoList,
        const LogFn& log,
        std::atomic<bool>& stopReq,
        const char* tag)
    {
        if (ch.Kind() == stcisp::ChannelKind::Hid)
            return ConnectHid(ch, deviceId, rxBuf, protoList, log, stopReq, tag);
        return ConnectSerial(ch, deviceId, rxBuf, protoList, log, stopReq, tag);
    }

    // =======================================================================
    //  Reporting
    // =======================================================================

    void PrintMcuInfo(const McuStatus& st, const std::string& protoName,
        const LogFn& log)
    {
        log("--------------------------------------------------");
        log(std::string("芯片型号 : ") + ChipNameFromTable(st.magic));
        log(std::string("Magic    : ") + Hex(st.magic, 4));
        if (!protoName.empty())
            log(std::string("协议     : ") + protoName);

        // BSL stepping byte is an ASCII letter on every family that reports it.
        std::string stepping;
        if (st.bslStepping >= 0x20 && st.bslStepping < 0x7F)
            stepping = std::string(1, (char)st.bslStepping);
        log(std::string("BSL 版本 : ") +
            std::to_string(st.bslVersion >> 4) + "." +
            std::to_string(st.bslVersion & 0x0F) + stepping);

        {
            uint32_t ip = st.clockHz / 1000000;
            uint32_t fp = st.clockHz % 1000000;
            char fb[8];
            std::snprintf(fb, sizeof(fb), "%06u", fp);
            fb[3] = '\0';
            log(std::string("时钟频率 : ") + std::to_string(ip) + "." + fb +
                " MHz");
        }

        log("--------------------------------------------------");
    }

    namespace {

        // The chip-family summary block shared by detect and burn.
        void PrintProtoParams(const std::string& protoName, const McuStatus& st,
            const LogFn& log)
        {
            // Rebuilding a protocol just to read Params() would be wasteful, so
            // the caller logs the params it already has; this helper only
            // covers the EEPROM sizing line, which needs msr[4].
            if (protoName == "stc32" || protoName == "stc8g" ||
                protoName == "stc8") {
                uint8_t msr4 = st.msr[4];
                if (msr4 > 0 && msr4 < 0xFF) {
                    std::string chipName = ChipNameFromTable(st.magic);
                    uint32_t totalFlash = GetChipTotalFlash(chipName, st.magic);
                    uint32_t codeSize = (uint32_t)msr4 * 256;
                    if (codeSize <= totalFlash) {
                        uint32_t eeSize = totalFlash - codeSize;
                        log(std::string("当前 EEPROM: ") +
                            std::to_string(eeSize) + " 字节（程序区 " +
                            std::to_string(codeSize) + " 字节）");
                    }
                    else {
                        log("当前 EEPROM: 值异常");
                    }
                }
                else {
                    log("当前 EEPROM: 未设置");
                }
            }
        }

        // Clamp the requested frequency to what the family can actually reach.
        //
        // Every family now tops out at or below the vendor's 45 MHz ceiling;
        // STC8G and STC15 reach only 35 MHz, so a request above that is brought
        // down with a note rather than attempted.
        uint32_t ClampFrequency(const std::string& chipName, uint32_t freq,
            const LogFn& log)
        {
            auto starts = [&](const char* p) {
                return chipName.rfind(p, 0) == 0;
            };

            bool isStc8g = starts("STC8G") || starts("AI8G") || starts("JX8G");
            bool isStc15 = IsStc15Family(chipName);
            int freqMax = kNumFreq - 1;
            if (isStc8g)      freqMax = kFreqMaxIdxStc8G;
            else if (isStc15) freqMax = kFreqMaxIdxStc15;

            if (freq > kFreqTable[freqMax]) {
                log("[频率] 该系列最高 " +
                    FormatDouble(kFreqTable[freqMax] / 1e6, 4) +
                    " MHz，目标频率已下调");
                return kFreqTable[freqMax];
            }
            return freq;
        }

        // Legacy families have an 8-bit BRT register with a fixed 16x divider,
        // which cannot reach past 115200. Clamping here rather than failing
        // mid-burn matches what the UI did.
        uint32_t ClampBaud(const std::string& protoName, uint32_t baud,
            const LogFn& log)
        {
            if (IsLegacyBaudProto(protoName) && baud > 115200) {
                log("[2/7] 该系列最高 115200，已从 " + std::to_string(baud) +
                    " 下调");
                return 115200;
            }
            return baud;
        }

        std::unique_ptr<stcisp::IChannel> MakeChannel(bool useHid)
        {
            if (useHid)
                return std::unique_ptr<stcisp::IChannel>(new stcisp::HidChannel());
            return std::unique_ptr<stcisp::IChannel>(new stcisp::SerialChannel());
        }

    } // namespace

    // =======================================================================
    //  Detect
    // =======================================================================

    int RunDetect(const BurnOptions& opts, const LogFn& log)
    {
        auto ch = MakeChannel(opts.useHid);
        std::atomic<bool> stopReq{ false };
        std::vector<uint8_t> rxBuf;

        auto probeList = MakeProbeList(opts.useHid);

        log(opts.useHid ? "检测中（USB HID）..."
                        : "检测中（最多 500 轮自动重启）...");

        auto r = OpenSession(*ch, opts.deviceId, rxBuf, probeList, log, stopReq,
            "检测");

        if (r.openFailed) {
            if (ch->IsOpen()) ch->Close();
            log(opts.useHid ? "打开 HID 设备失败，已停止"
                            : "打开串口失败，已停止");
            return 1;
        }

        if (!r.got) {
            if (ch->IsOpen()) ch->Close();
            log("未检测到芯片");
            return 1;
        }

        // Restore the parity the detected family needs before signing off, so
        // the chip leaves the ISP monitor in a defined state. The 0xFF packet
        // is the reset-and-run command.
        bool wantParity = true;
        for (auto& p : probeList) {
            if (p->Params().name == r.protoName) {
                wantParity = p->UseEvenParity();
                break;
            }
        }
        if (ch->IsOpen() && ch->EvenParity() != wantParity) {
            ch->SetParity(wantParity);
        }
        if (ch->IsOpen()) {
            auto reset = probeList.empty()
                ? std::vector<uint8_t>{}
                : probeList.front()->ProbeBuild({ 0xFF });
            if (!reset.empty()) ch->Write(reset);
            ch->Close();
        }

        std::string chipName = ChipNameFromTable(r.st.magic);
        r.chipName = chipName;

        // The 8051U is documented as HID-only, but it answered a serial probe,
        // and answering is the proof that matters. Report the discrepancy and
        // carry on rather than refusing: the transfer is the experiment.
        if (IsHidOnlyChip(chipName) && !opts.useHid) {
            log("[注意] " + chipName +
                " 按官方手册只有 USB HID 下载方式，但它在串口上应答了探测。");
            log("[注意] 继续按串口流程处理。这是手册与实际不符的情况，"
                "结果需要自行确认。");
        }

        log("检测成功: " + chipName);
        PrintMcuInfo(r.st, r.protoName, log);
        PrintProtoParams(r.protoName, r.st, log);
        return 0;
    }

    // =======================================================================
    //  Burn
    // =======================================================================

    int RunBurn(const BurnOptions& opts, const std::vector<uint8_t>& firmware,
        const LogFn& log, const ProgressFn& progress)
    {
        auto ch = MakeChannel(opts.useHid);
        std::atomic<bool> stopReq{ false };
        std::vector<uint8_t> rxBuf;
        std::unique_ptr<IStcProtocol> proto;

        auto fail = [&](const char* stage, const char* why) {
            log(std::string("[失败] ") + stage + " - " + why);
        };
        auto closeCh = [&]() {
            if (ch->IsOpen()) ch->Close();
        };

        auto probeList = MakeProbeList(opts.useHid);

        if (!opts.useHid)
            log("[1/7] 握手（最多 500 轮自动重启）...");

        auto r = OpenSession(*ch, opts.deviceId, rxBuf, probeList, log, stopReq,
            opts.useHid ? "探测" : "烧录");

        if (r.openFailed) {
            fail(opts.useHid ? "HID 设备" : "串口",
                "打开失败，请检查设备是否被占用");
            closeCh();
            return 1;
        }

        if (!r.got) {
            fail(opts.useHid ? "探测" : "握手",
                opts.useHid ? "未检测到芯片（确认已按住 BOOT 重新上电）"
                            : "500 轮后仍未检测到芯片（检查类型是否选对）");
            closeCh();
            return 1;
        }

        auto st = r.st;
        log(opts.useHid ? "[1/7] 已识别芯片" : "[1/7] 握手成功");
        progress(5);

        bool wantParity = true;
        for (auto& p : probeList) {
            if (p->Params().name == r.protoName) {
                wantParity = p->UseEvenParity();
                break;
            }
        }
        if (ch->IsOpen() && ch->EvenParity() != wantParity) {
            if (!ch->SetParity(wantParity)) log("[警告] 切换校验位失败");
        }

        std::string chipName = ChipNameFromTable(st.magic);

        if (IsUnsupportedForTransport(chipName, opts.useHid,
                opts.allowHidOnlySerial)) {
            fail("提示", "该芯片没有串口 ISP 模式，请改用 --hid，"
                         "或加 --force-serial 强制尝试");
            closeCh();
            return 1;
        }

        if (IsHidOnlyChip(chipName) && !opts.useHid) {
            log("[警告] " + chipName +
                " 按文档说明没有串口 ISP 监视器，本次按串口流程继续。");
        }

        // Prefer the implementation that identified itself during probing; fall
        // back to the name-derived one, then to the family override.
        proto = CloneProtocolByName(r.protoName, opts.useHid);
        if (!proto) proto = CreateProtocol(chipName, opts.useHid);
        if (!proto && opts.protoOverride >= 0)
            proto = MakeProtocol(opts.protoOverride, opts.useHid);
        if (!proto) {
            fail("协议", "无法为该芯片选择协议实现");
            closeCh();
            return 1;
        }

        proto->m_status = st;
        proto->m_status.userTargetFreq = opts.targetFreq;
        proto->m_status.wantExternalClock = opts.clockExternal;

        uint32_t targetFreq = ClampFrequency(chipName, opts.targetFreq, log);
        proto->m_status.userTargetFreq = targetFreq;

        uint32_t totalFlash = GetChipTotalFlash(chipName, st.magic);

        if (proto->SupportsEepromSplit()) {
            proto->SetEepromSplit(opts.eepromBytes, totalFlash);
        }

        PrintMcuInfo(st, proto->Params().name, log);

        {
            auto pp = proto->Params();
            log("块大小   : " + std::to_string(pp.blockSize) + " 字节");
            log(std::string("IAP 等待 : 0x") + Hex(pp.iapWait));
            log(std::string("5A A5    : ") + (pp.need5AA5 ? "需要" : "不需要"));
        }
        PrintProtoParams(proto->Params().name, st, log);

        if (proto->SupportsEepromSplit()) {
            if (opts.eepromBytes > 0) {
                log("EEPROM   : " + FormatDouble(opts.eepromBytes / 1024.0, 1) +
                    " K（将写入，程序区 " +
                    std::to_string(totalFlash - opts.eepromBytes) + " 字节）");
            }
            else {
                log("EEPROM   : 自动（保留芯片设置）");
            }
        }

        // ---- step 2: clock parameters / baud rate ----
        if (opts.useHid) {
            // The USB ISP interface needs no wakeup and has no baud rate to
            // negotiate, so this step only primes the option-block trim values.
            log("[2/7] 准备时钟参数...");
            if (!proto->Handshake(*ch, rxBuf, 0, log)) {
                fail("[2/7] 参数", "失败");
                closeCh();
                return 1;
            }
            log("[2/7] 参数就绪");
            progress(15);

            rxBuf.clear();
            ch->Flush();
            SleepMs(30);

            // 0x01 BAUD_SET must be sent before 0x05 / erase. There is no baud
            // rate to negotiate over USB, but the ISP still expects this
            // exchange; skipping it makes the subsequent erase command time out
            // without a reply.
            log("[2/7] 设置波特率 (0x01)...");
            {
                std::vector<uint8_t> pl = { 0x01, 0x00, 0x00, 0x00,
                                            0x00, 0x00, 0x00, 0x80 };
                auto tx = proto->ProbeBuild(pl);
                ch->Write(tx);
                log(std::string("[发送] 0x01: ") + HexDump(tx));

                std::vector<uint8_t> rr;
                if (!proto->ProbeWait(*ch, rxBuf, rr, 2000)) {
                    fail("[2/7] 0x01", "无响应");
                    closeCh();
                    return 1;
                }
                log(std::string("[接收] ") + HexDump(rr));
                if (rr.empty() || rr[0] != 0x01) {
                    fail("[2/7] 0x01", "响应包错误");
                    closeCh();
                    return 1;
                }
            }
            log("[2/7] 波特率设置完成");
        }
        else {
            log("[2/7] 校准并切换波特率...");
            uint32_t baud = ClampBaud(proto->Params().name, opts.baudRate, log);

            if (!proto->Handshake(*ch, rxBuf, baud, log)) {
                fail("[2/7] 校准", "失败");
                closeCh();
                return 1;
            }
            log("[2/7] 已切到 " + std::to_string(baud));
            progress(15);

            rxBuf.clear();
            ch->Flush();
            SleepMs(30);
        }

        // ---- step 3: prepare programming (0x05), where the family has it ----
        if (NeedsPrepare05(proto->Params().name)) {
            log("[3/7] 准备编程 (0x05)...");
            std::vector<uint8_t> pl;
            pl.push_back(0x05);
            if (st.bslVersion >= 0x72) {
                pl.push_back(0x00); pl.push_back(0x00);
                pl.push_back(0x5A); pl.push_back(0xA5);
            }
            auto tx = proto->ProbeBuild(pl);
            ch->Write(tx);
            log(std::string("[发送] 0x05: ") + HexDump(tx));

            std::vector<uint8_t> rr;
            if (!proto->ProbeWait(*ch, rxBuf, rr, 2000)) {
                fail("[3/7] 0x05", "无响应");
                closeCh();
                return 1;
            }
            log(std::string("[接收] ") + HexDump(rr));
            if (rr.empty() || rr[0] != 0x05) {
                fail("[3/7] 0x05", "响应包错误");
                closeCh();
                return 1;
            }
            log("[3/7] 准备编程成功");
        }
        else {
            log("[3/7] 跳过（该系列无 0x05 步骤）");
        }
        progress(25);

        // ---- step 4: erase ----
        log("[4/7] 擦除...");
        if (!proto->EraseFlash(*ch, rxBuf, firmware.size(), firmware.size(), log)) {
            fail("[4/7] 擦除", "失败");
            closeCh();
            return 1;
        }
        log("[4/7] 擦除成功");
        progress(35);

        // ---- step 5: program ----
        log("[5/7] 写入代码...");
        if (!proto->ProgramFlash(*ch, rxBuf, firmware, progress, log)) {
            fail("[5/7] 写块", "失败");
            closeCh();
            return 1;
        }
        log("[5/7] 写块完成");

        // ---- step 6: option bytes ----
        log("[6/7] 写入选项...");
        {
            // Option bytes carry the watchdog, reset behaviour and clock
            // configuration; a wrong value can leave the chip unable to start,
            // so echo exactly what is about to be written.
            std::string cs = proto->ClockSourceText();
            std::string msr = "MSR: " + Hex(st.msr[0]) + " " + Hex(st.msr[1]) +
                " " + Hex(st.msr[2]) + " " + Hex(st.msr[3]);
            if (cs.empty())
                log("[6/7] 将回写 " + msr);
            else
                log("[6/7] 将回写 " + msr + " （时钟源=" + cs + "）");
        }
        if (!proto->ProgramOptions(*ch, rxBuf, log)) {
            fail("[6/7] 选项", "失败");
            closeCh();
            return 1;
        }
        log("[6/7] 选项写入成功");
        progress(100);

        // ---- step 7: run ----
        log("[7/7] 烧录完成！");
        proto->Disconnect(*ch);
        closeCh();
        return 0;
    }

} // namespace newisp
