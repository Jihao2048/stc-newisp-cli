#pragma once
// Session layer: the bring-up, detect and burn sequences.
//
// These were MainWindow::DetectWorker / MainWindow::BurnWorker / OpenSession in
// the WinUI build, tangled with XAML controls and a dispatcher. Here they are
// plain functions over IChannel and IStcProtocol, with logging and progress
// delivered through callbacks, so both the CLI and any future GUI can drive
// them.

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "newisp/IChannel.h"
#include "newisp/stc_protocol.h"
#include "newisp/chip_logic.h"

namespace newisp {

    // Everything the session needs that used to come from the UI controls.
    struct BurnOptions {
        std::string deviceId;          // COM name / tty path / HID interface path
        bool        useHid = false;    // transport selection

        uint32_t    baudRate = 115200;
        uint32_t    targetFreq = 24000000;   // IRC frequency to program
        bool        clockExternal = false;   // STC12-family clock source
        uint32_t    eepromBytes = 0;         // 0 = keep the chip's setting

        // Chip family selected on the command line, used only as a fallback
        // when probing identifies nothing. -1 means "no preference".
        int         protoOverride = -1;

        // When false, the burn sequence probes for the chip first; when true it
        // uses protoOverride directly without a wakeup.
        bool        skipProbe = false;

        // Proceed even for a part the documentation says has no serial ISP
        // monitor (the 8051U family).
        //
        // The rule is inherited from the reference material rather than
        // measured here, and the probe is the real test: a chip that answers
        // the wakeup can be programmed whatever the manual claims. This flag
        // turns an up-front refusal into a warning so that can be established
        // on real hardware.
        bool        allowHidOnlySerial = false;
    };

    // Result of a detect or the probe phase of a burn.
    struct SessionResult {
        bool got = false;           // a chip answered
        bool openFailed = false;    // the port/device could not be opened at all
        stc::McuStatus st{};
        std::string protoName;      // protocol that identified itself
        std::string chipName;       // resolved part name
    };

    using LogFn = std::function<void(const std::string&)>;
    using ProgressFn = std::function<void(int)>;

    // Bring-up for the serial transport: the chip may still be running user
    // code, so DTR is pulsed and 0x7F is spammed until the ISP monitor answers.
    // Can take many rounds and takes seconds.
    SessionResult ConnectSerial(stcisp::IChannel& ch,
        const std::string& deviceId,
        std::vector<uint8_t>& rxBuf,
        std::vector<std::unique_ptr<stc::IStcProtocol>>& protoList,
        const LogFn& log,
        std::atomic<bool>& stopReq,
        const char* tag);

    // Bring-up for the HID transport: the chip is already in the ISP monitor
    // once it enumerates, so each protocol's init packet is sent once.
    SessionResult ConnectHid(stcisp::IChannel& ch,
        const std::string& deviceId,
        std::vector<uint8_t>& rxBuf,
        std::vector<std::unique_ptr<stc::IStcProtocol>>& protoList,
        const LogFn& log,
        std::atomic<bool>& stopReq,
        const char* tag);

    // Dispatch to the right bring-up for the channel's kind.
    SessionResult OpenSession(stcisp::IChannel& ch,
        const std::string& deviceId,
        std::vector<uint8_t>& rxBuf,
        std::vector<std::unique_ptr<stc::IStcProtocol>>& protoList,
        const LogFn& log,
        std::atomic<bool>& stopReq,
        const char* tag);

    // Print the chip identification block that both detect and burn emit.
    void PrintMcuInfo(const stc::McuStatus& st, const std::string& protoName,
        const LogFn& log);

    // Detect only: open, probe, report, close without touching flash.
    int RunDetect(const BurnOptions& opts, const LogFn& log);

    // Full burn: probe, calibrate, erase, write, write options, disconnect.
    int RunBurn(const BurnOptions& opts, const std::vector<uint8_t>& firmware,
        const LogFn& log, const ProgressFn& progress);

} // namespace newisp
