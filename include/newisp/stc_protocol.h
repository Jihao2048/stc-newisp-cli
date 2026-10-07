#pragma once
// STC ISP protocol base: packet framing, wait/pulse helpers.
//
// Ported from the WinUI build's stc_protocol.h. Three things changed:
//
//   * <windows.h> is gone. The only thing it was used for here was
//     GetTickCount64, which is now newisp::NowMs().
//   * std::wstring became std::string holding UTF-8, so the Chinese log
//     messages reach a terminal intact on all three platforms.
//   * The helpers that were inline free functions in a header (HexW, HexDump)
//     now live in newisp/text.h so the same code is shared with the CLI.

#include <string>
#include <vector>
#include <cstdint>
#include <functional>

#include "newisp/IChannel.h"
#include "newisp/text.h"

namespace stc {

    using LogFn = std::function<void(const std::string&)>;
    using ProgressFn = std::function<void(int)>;

    // Bring the shared formatters into this namespace.
    //
    // The WinUI build defined HexW and HexDump inside stc::, so every protocol
    // file calls them unqualified. They now live in newisp:: because the CLI
    // needs them too; these using-declarations keep the ported protocol code
    // textually identical to the original.
    using newisp::HexDump;
    using newisp::Hex;

    // The original spelling for the zero-padded hex formatter.
    inline std::string HexW(uint32_t v, int width = 2) { return Hex(v, width); }

    // The original spelling for the monotonic millisecond counter. The WinUI
    // build called the Win32 function of this name directly from a few
    // protocol methods, so keeping the name avoids editing them.
    inline uint64_t GetTickCount64() { return newisp::NowMs(); }

    // ========================================================================
    //  Common structures
    // ========================================================================
    struct McuStatus {
        uint16_t magic = 0;
        uint8_t  bslVersion = 0;
        uint8_t  bslStepping = 0;
        uint32_t clockHz = 0;
        uint32_t userTargetFreq = 0;
        // User-selected clock source: false = internal IRC, true = external
        // crystal. Set by the CLI, applied by the protocol layer when it writes
        // the option bytes.
        bool     wantExternalClock = false;
        std::vector<uint8_t> raw;
        uint8_t  msr[5] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
    };

    struct ProtocolParams {
        size_t      blockSize = 64;
        uint8_t     iapWait = 0x98;
        bool        need5AA5 = false;
        std::string name = "unknown";
    };

    // ========================================================================
    //  Protocol base class
    // ========================================================================
    class IStcProtocol {
    public:
        virtual ~IStcProtocol() = default;

        McuStatus m_status;

        virtual ProtocolParams Params() const = 0;
        virtual bool UseEvenParity() const { return true; }

        // ---- probe interface ----
        bool ProbeParse(const std::vector<uint8_t>& buf,
            std::vector<uint8_t>& payload,
            size_t& consumed) const
        {
            return ParsePacket(buf, payload, consumed);
        }

        std::vector<uint8_t> ProbeBuild(const std::vector<uint8_t>& payload,
            uint8_t dir = 0x6A,
            size_t epilogueLen = 0) const
        {
            return BuildPacket(payload, dir, epilogueLen);
        }

        bool ProbeWait(stcisp::IChannel& uart, std::vector<uint8_t>& rxBuf,
            std::vector<uint8_t>& outPayload, uint32_t timeoutMs) const
        {
            return WaitPacket(uart, rxBuf, outPayload, timeoutMs);
        }

        // ---- abstract flow ----
        virtual bool ParseStatus(const std::vector<uint8_t>& payload, McuStatus& out) = 0;
        virtual bool Handshake(stcisp::IChannel&, std::vector<uint8_t>&,
            uint32_t, const LogFn&) {
            return true;
        }
        virtual bool EraseFlash(stcisp::IChannel& uart, std::vector<uint8_t>& rxBuf,
            size_t codeSize, size_t flashSize,
            const LogFn& log) = 0;
        virtual bool ProgramFlash(stcisp::IChannel& uart, std::vector<uint8_t>& rxBuf,
            const std::vector<uint8_t>& data,
            const ProgressFn& progress, const LogFn& log) = 0;
        virtual bool ProgramOptions(stcisp::IChannel& uart, std::vector<uint8_t>& rxBuf,
            const LogFn& log) = 0;
        virtual void Disconnect(stcisp::IChannel& uart) = 0;

        // ---- optional capabilities ----
        //
        // Only the STC32 relatives (STC32 / STC8H / 8051U / 8A8K / 8A2K and
        // the STC8G/STC8 variants) expose a user-selectable EEPROM split in
        // their option block, so callers ask through these rather than
        // downcasting.
        virtual bool SupportsEepromSplit() const { return false; }
        virtual void SetEepromSplit(uint32_t eepromBytes, uint32_t totalFlash) {
            (void)eepromBytes; (void)totalFlash;
        }

        // Human-readable clock source for the option block, or an empty string
        // when the concept does not apply.
        //
        // Only the STC89/12/15 generation stores a clock-source bit in its
        // option bytes. From the STC8/32 families onward there is no external
        // crystal option at all, so those protocols leave this empty and the
        // CLI simply omits the field. Returning empty is therefore meaningful:
        // it means "this family has no such setting", not "unknown".
        virtual std::string ClockSourceText() const { return std::string(); }

        // ---- packet framing ----
        virtual std::vector<uint8_t> BuildPacket(const std::vector<uint8_t>& payload,
            uint8_t dir = 0x6A,
            size_t epilogueLen = 0) const
        {
            std::vector<uint8_t> p;
            p.reserve(payload.size() + 8 + epilogueLen);
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

        virtual bool ParsePacket(const std::vector<uint8_t>& buf,
            std::vector<uint8_t>& payload,
            size_t& consumed) const
        {
            for (size_t i = 0; i + 6 < buf.size(); ++i) {
                if (buf[i] != 0x46 || buf[i + 1] != 0xB9) continue;
                uint8_t dir = buf[i + 2];
                if (dir != 0x68 && dir != 0x6A) continue;
                uint16_t len = ((uint16_t)buf[i + 3] << 8) | buf[i + 4];
                if (len < 6) continue;
                size_t payloadLen = len - 6;
                size_t end = i + 5 + payloadLen + 2;
                if (end >= buf.size()) continue;
                if (buf[end] != 0x16) continue;
                uint16_t cs = 0;
                for (size_t j = i + 2; j < i + 5 + payloadLen; ++j) cs += buf[j];
                uint16_t csIn = ((uint16_t)buf[end - 2] << 8) | buf[end - 1];
                if (cs != csIn) continue;
                payload.assign(buf.begin() + i + 5, buf.begin() + i + 5 + payloadLen);
                consumed = end + 1;
                return true;
            }
            return false;
        }

        bool WaitPacket(stcisp::IChannel& uart, std::vector<uint8_t>& rxBuf,
            std::vector<uint8_t>& outPayload, uint32_t timeoutMs) const
        {
            uint64_t start = newisp::NowMs();
            while ((newisp::NowMs() - start) < timeoutMs) {
                std::vector<uint8_t> pl; size_t cons = 0;
                if (ParsePacket(rxBuf, pl, cons)) {
                    rxBuf.erase(rxBuf.begin(), rxBuf.begin() + (ptrdiff_t)cons);
                    outPayload = pl;
                    return true;
                }
                auto chunk = uart.Read(512, 0);
                if (!chunk.empty()) rxBuf.insert(rxBuf.end(), chunk.begin(), chunk.end());
                newisp::SleepMs(2);
            }
            return false;
        }

        bool PulseUntilData(stcisp::IChannel& uart, uint8_t ch,
            uint32_t timeoutMs, std::vector<uint8_t>& rxBuf,
            uint32_t intervalMs = 30) const
        {
            {
                std::vector<uint8_t> tmp; size_t cons = 0;
                if (ParsePacket(rxBuf, tmp, cons)) return true;
            }
            {
                auto c = uart.Read(512, 0);
                if (!c.empty()) {
                    rxBuf.insert(rxBuf.end(), c.begin(), c.end());
                    return true;
                }
            }

            uint64_t start = newisp::NowMs();
            std::vector<uint8_t> one(1, ch);
            while ((newisp::NowMs() - start) < timeoutMs) {
                uart.Write(one);
                newisp::SleepMs(intervalMs);
                auto c = uart.Read(512, 0);
                if (!c.empty()) {
                    rxBuf.insert(rxBuf.end(), c.begin(), c.end());
                    newisp::SleepMs(20);
                    auto c2 = uart.Read(512, 0);
                    if (!c2.empty()) rxBuf.insert(rxBuf.end(), c2.begin(), c2.end());
                    return true;
                }
            }
            return false;
        }

    protected:
        uint8_t GetIapDelay(uint32_t clockHz) const {
            if (clockHz < 1E6)  return 0x87;
            if (clockHz < 2E6)  return 0x86;
            if (clockHz < 3E6)  return 0x85;
            if (clockHz < 6E6)  return 0x84;
            if (clockHz < 12E6) return 0x83;
            if (clockHz < 20E6) return 0x82;
            if (clockHz < 24E6) return 0x81;
            return 0x80;
        }
    };

} // namespace stc
