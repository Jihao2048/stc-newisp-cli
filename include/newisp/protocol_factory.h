#pragma once
#include <memory>
#include <string>
#include "newisp/chip_table.h"
#include "newisp/protocol_stc89.h"
#include "newisp/protocol_stc12.h"
#include "newisp/protocol_stc15.h"
#include "newisp/protocol_stc8.h"

namespace stc {

    // The STC8G protocol pair, chosen by transport.
    inline std::unique_ptr<IStcProtocol> Stc8GProto(bool useHid)
    {
        if (useHid) return std::unique_ptr<IStcProtocol>(new ProtocolStc8GHid());
        return std::unique_ptr<IStcProtocol>(new ProtocolStc8GUart());
    }

    // The STC32 protocol pair, chosen by transport.
    inline std::unique_ptr<IStcProtocol> Stc32Proto(bool useHid)
    {
        if (useHid) return std::unique_ptr<IStcProtocol>(new ProtocolStc32Hid());
        return std::unique_ptr<IStcProtocol>(new ProtocolStc32Uart());
    }

    // The STC8 protocol pair, chosen by transport. These parts have no USB
    // interface, so callers pass false; the HID variant exists only so the
    // pair is complete.
    inline std::unique_ptr<IStcProtocol> Stc8Proto(bool useHid)
    {
        if (useHid) return std::unique_ptr<IStcProtocol>(new ProtocolStc8Hid());
        return std::unique_ptr<IStcProtocol>(new ProtocolStc8Uart());
    }

    // Map a chip name to a protocol implementation.
    //
    // Two builds of the STC32/STC8G/STC8 protocols exist -- one per transport,
    // see protocol_stc8.h -- so useHid selects which set to instantiate. The
    // older serial-only families (STC89/90/10/11/12/15) have a single
    // implementation and ignore the flag.
    //
    // The 8G family is the one case that needs a second condition: only its
    // 08A parts have USB, so useHid is honoured there only when the name
    // matches. The other 8G parts fall through to the serial build.
    inline std::unique_ptr<IStcProtocol> CreateProtocol(const std::string& name,
        bool useHid = false)
    {
        auto starts = [&](const char* p) { return name.rfind(p, 0) == 0; };
        auto has = [&](const char* p) { return name.find(p) != std::string::npos; };

        // STC89/90 老系列
        if (starts("STC89") || starts("STC90"))
            return std::make_unique<ProtocolStc89A>();   // 新 89/90 用 0x50 状态包

        // STC12A：12C/LE + "052"
        if ((starts("STC12C") || starts("STC12LE")) && has("052"))
            return std::make_unique<ProtocolStc12A>();

        // STC12B：12C/LE + "52" 或 "56"
        if ((starts("STC12C") || starts("STC12LE")) && (has("52") || has("56")))
            return std::make_unique<ProtocolStc12B>();

        // STC10/11/12 通用
        if (starts("STC10") || starts("STC11") || starts("STC12") ||
            starts("IAP10") || starts("IAP11") || starts("IAP12"))
            return std::make_unique<ProtocolStc12>();

        // STC15A：15F/L + 100~207
        if ((starts("STC15F") || starts("STC15") ||
            starts("IAP15F") || starts("IAP15"))
            && (has("100") || has("101") || has("102") || has("103") ||
                has("104") || has("105") || has("107") ||
                has("204") || has("205") || has("207")))
            return std::make_unique<ProtocolStc15A>();

        // STC15：其它 15 系列
        if (starts("STC15") || starts("IAP15") || starts("IRC15"))
            return std::make_unique<ProtocolStc15>();

        // STC8G / AI8G / JX8G
        //
        // Parts with no USB interface can never reach here with useHid set:
        // HID mode only lists devices that actually enumerate as the STC
        // bootloader interface, so a serial-only part is simply not selectable.
        // That is why this branch -- and the ones below -- use useHid directly
        // instead of second-guessing it.
        if (starts("STC8G") || starts("AI8G") || starts("JX8G"))
            return Stc8GProto(useHid);

        // STC8H / AI8H -- 8H1K parts are served by the 8G protocol.
        if (starts("STC8H") || starts("AI8H")) {
            if (has("1K") && has("K1")) return Stc8GProto(useHid);
            return Stc32Proto(useHid);
        }

        // STC32 / 8051U.
        if (starts("STC32") || starts("AI32") ||
            starts("STC8051U") || starts("AI8051U"))
            return Stc32Proto(useHid);

        // STC16 / 8A8K / 8A2K / STC8F / 8A / 8C / GX8S -- these have no USB
        // interface, so useHid is false by construction; they always run over
        // serial.
        if (starts("STC16") || starts("AI16") ||
            starts("STC8A8K") || starts("AI8A8K") ||
            starts("STC8A2K") || starts("AI8A2K"))
            return Stc32Proto(false);

        if (starts("STC8F") || starts("STC8A") || starts("STC8C") ||
            starts("AI8C") || starts("GX8S"))
            return Stc8Proto(false);

        return nullptr;
    }

} // namespace stc
