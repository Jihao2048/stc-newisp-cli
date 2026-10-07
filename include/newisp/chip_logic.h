#pragma once
// Chip-family classification and device-list helpers.
//
// These are pure functions: they depend on chip names, protocol names and
// device descriptors, but never on XAML controls. Keeping them out of
// MainWindow.xaml.cpp lets the window code read as UI plumbing, and makes the
// classification rules testable and greppable in one place.
//
// The tables here mirror what the protocol layer implements; see
// protocol_factory.h for the authoritative chip-name -> protocol mapping.

#include <string>
#include <vector>
#include <cstdint>
#include <memory>

#include "newisp/IChannel.h"
#include "newisp/hid_channel.h"
#include "newisp/chip_table.h"
#include "newisp/stc_protocol.h"
#include "newisp/protocol_factory.h"

namespace newisp {

    // ========================================================================
    //  Frequency / baud tables
    // ========================================================================

    // Selectable IRC frequencies, in the order the menu lists them.
    //
    // This is the vendor-validated range, which stops at 45 MHz. It is now the
    // whole selectable set: the overclock band that used to sit above it has
    // been removed, so nothing here can ask the IRC to run beyond what the
    // vendor has validated. 48 MHz in particular was measured to *drop* the
    // clock rather than raise it, and the entries above it were extrapolated
    // from a fitted curve.
    inline const uint32_t kFreqTable[] = {
        5529600, 6000000, 11059200, 12000000,
        22118400, 24000000, 33177600, 35000000,
        40000000, 45000000
    };
    inline const int kNumFreq = 10;

    // STC8G / JX8G top out around 35 MHz: index 7 is the last safe entry.
    inline const int kFreqMaxIdxStc8G = 7;

    // STC15 also stops at 35 MHz. Indices 8..9 (40/45 MHz) are unsafe for that
    // family and must not be selectable.
    inline const int kFreqMaxIdxStc15 = 7;

    // Serial baud rates, in slider order.
    inline const uint32_t kBaudTable[] = {
        2400, 4800, 9600, 19200, 38400, 115200, 230400, 460800
    };
    inline const int kNumBaud = 8;

    // STC89/90/10/11/12 are locked to 115200 (index 5).
    //
    // Their BRT register is 8 bits wide with a fixed 16x divider, i.e.
    // baud = clock / (16 * (256 - BRT)). At 24 MHz, BRT = 0xF3 gives 115200;
    // reaching 230400 needs (256 - BRT) = 6.51, and the rounding error exceeds
    // 7% (UART tolerance is about 3%), so the handshake would always fail.
    //
    // stcgal behaves the same way: its calculate_baud() raises
    // "requested baudrate cannot be set" for this case (protocols.py:1123).
    // We clamp in the UI instead, so the user cannot pick a rate that fails
    // mid-burn.
    inline const int kBaudMaxIdxStc12 = 5;

    // ========================================================================
    //  Clock source
    // ========================================================================

    // The overclock table that used to live here has been removed.
    //
    // It offered 46-50 MHz, all of it outside the vendor-validated range. Only
    // 46 and 47 MHz were ever measured to work, 48 MHz was measured to *drop*
    // the clock, and 49/50 MHz were extrapolated from a fitted curve rather
    // than tested. A clock the chip cannot actually reach is worse than no
    // option at all: the program runs at an unknown speed and the only symptom
    // is timing that is subtly wrong. The selectable range now stops at the
    // vendor's own 45 MHz ceiling, which is what kFreqTable holds.

    // STC12 family: clock source replaces the frequency menu entirely.
    // 0 = internal IRC, 1 = external crystal.
    inline const char* kClockSrcText[] = { "内部 IRC", "外部晶振" };

    // ========================================================================
    //  Chip-family menu indices (order of GetProtoItems in MainWindow)
    // ========================================================================
    inline const int kProtoIdxStc89 = 0;
    inline const int kProtoIdxStc12 = 1;
    inline const int kProtoIdxStc15 = 2;
    inline const int kProtoIdxStc8G = 3;
    inline const int kProtoIdxStc32 = 4;
    inline const int kProtoIdxStc8 = 5;

    // ========================================================================
    //  Chip-name predicates
    // ========================================================================

    inline bool StartsWith(const std::string& s, const char* prefix)
    {
        return s.rfind(prefix, 0) == 0;
    }

    inline std::string ProtoIdxToChipName(int idx)
    {
        switch (idx) {
        case kProtoIdxStc89: return "STC89";
        case kProtoIdxStc12: return "STC12";
        case kProtoIdxStc15: return "STC15";
        case kProtoIdxStc8G: return "STC8G";
        case kProtoIdxStc32: return "STC32";
        case kProtoIdxStc8:  return "STC8";
        default:             return "STC32";
        }
    }

    // STC10/11/12 need a clock-source choice instead of a frequency.
    inline bool IsStc12Family(const std::string& chipName)
    {
        return StartsWith(chipName, "STC10") || StartsWith(chipName, "STC11") ||
            StartsWith(chipName, "STC12") || StartsWith(chipName, "IAP10") ||
            StartsWith(chipName, "IAP11") || StartsWith(chipName, "IAP12");
    }

    inline bool IsStc8gFamily(const std::string& chipName)
    {
        return StartsWith(chipName, "STC8G") || StartsWith(chipName, "AI8G") ||
            StartsWith(chipName, "JX8G");
    }

    inline bool IsStc15Family(const std::string& chipName)
    {
        return StartsWith(chipName, "STC15") || StartsWith(chipName, "IAP15") ||
            StartsWith(chipName, "IRC15");
    }

    // STC89/90 always run from an external crystal, so the frequency menu is
    // irrelevant for them.
    inline bool IsExternalCrystalFamily(const std::string& chipName)
    {
        return StartsWith(chipName, "STC89") || StartsWith(chipName, "STC90");
    }

    // The 8051U family is documented as having no serial ISP monitor: the
    // vendor material says it is programmed through its factory USB HID
    // interface, and over HID it is a fully supported target.
    //
    // This predicate is kept because it is still the right thing to say in a
    // warning, but it no longer gates anything. See
    // IsUnsupportedForTransport for why.
    inline bool IsHidOnlyChip(const std::string& chipName)
    {
        return chipName.find("8051U") != std::string::npos ||
            chipName.find("8051u") != std::string::npos;
    }

    // True only when the chip should not be programmed over the active
    // transport.
    //
    // This now always returns false, and that is deliberate.
    //
    // The old rule refused to drive an 8051U over serial because the manual
    // says it has no serial ISP monitor. But the rule was inherited, never
    // measured here, and it is the wrong shape for the evidence: the probe
    // already answers the question. A wakeup byte that gets a framed, checksum-
    // valid status packet back proves the part is listening, whatever the
    // documentation says about that transport. A part that is not listening
    // fails the probe on its own, with a clear timeout, and nothing after it
    // runs.
    //
    // So the gate is gone and the probe does the deciding. The 8051U is still
    // reported as a HID-only part in a warning, because that is useful context
    // if the serial attempt then fails.
    //
    // The parameters are kept so callers read the same as before.
    inline bool IsUnsupportedForTransport(const std::string& chipName,
        bool useHid, bool /*allowHidOnlySerial*/ = false)
    {
        (void)chipName;
        (void)useHid;
        return false;
    }

    // True when the family is reachable over the factory USB HID ISP interface.
    //
    // The STC8F/8A/8C/GX8S group is served by ProtocolStc8, but none of those
    // parts has a USB interface, so index 5 is deliberately excluded even
    // though the HID probe list contains a ProtocolStc8 class for the 08A
    // parts of the 8G range.
    inline bool IsHidReachableProto(int idx)
    {
        return idx == kProtoIdxStc8G || idx == kProtoIdxStc32;
    }

    // Highest selectable frequency index for a family.
    inline int FreqMaxIdxFor(const std::string& chipName)
    {
        if (IsStc8gFamily(chipName)) return kFreqMaxIdxStc8G;
        if (IsStc15Family(chipName)) return kFreqMaxIdxStc15;
        return kNumFreq - 1;
    }

    // Family index shown in the chip-family menu, or -1 when unrecognised.
    inline int ProtoIdxFromChipName(const std::string& chipName)
    {
        if (chipName.empty()) return -1;
        if (StartsWith(chipName, "STC89") || StartsWith(chipName, "STC90"))
            return kProtoIdxStc89;
        if (StartsWith(chipName, "STC10") || StartsWith(chipName, "STC11") ||
            StartsWith(chipName, "STC12") || StartsWith(chipName, "IAP10") ||
            StartsWith(chipName, "IAP11") || StartsWith(chipName, "IAP12"))
            return kProtoIdxStc12;
        if (IsStc15Family(chipName))  return kProtoIdxStc15;
        if (IsStc8gFamily(chipName))  return kProtoIdxStc8G;
        if (StartsWith(chipName, "STC8H") || StartsWith(chipName, "AI8H") ||
            StartsWith(chipName, "STC32") || StartsWith(chipName, "AI32") ||
            StartsWith(chipName, "STC8051U") || StartsWith(chipName, "AI8051U") ||
            StartsWith(chipName, "STC8A8K") || StartsWith(chipName, "AI8A8K") ||
            StartsWith(chipName, "STC8A2K") || StartsWith(chipName, "AI8A2K"))
            return kProtoIdxStc32;
        if (StartsWith(chipName, "STC8F") || StartsWith(chipName, "STC8A") ||
            StartsWith(chipName, "STC8C") || StartsWith(chipName, "AI8C") ||
            StartsWith(chipName, "GX8S"))
            return kProtoIdxStc8;
        return -1;
    }

    // ========================================================================
    //  Protocol selection
    // ========================================================================

    // Build the STC32/STC8G/STC8 implementation for a transport.
    //
    // These three families have a separate class per transport (see
    // protocol_stc8.h), because their status framing and calibration differ.
    // The other families are serial-only and have a single implementation.
    inline std::unique_ptr<stc::IStcProtocol> MakeStc8FamilyProto(const std::string& name,
        bool useHid)
    {
        if (name == "stc8g")
            return useHid ? std::unique_ptr<stc::IStcProtocol>(new stc::ProtocolStc8GHid())
                          : std::unique_ptr<stc::IStcProtocol>(new stc::ProtocolStc8GUart());
        if (name == "stc8")
            return useHid ? std::unique_ptr<stc::IStcProtocol>(new stc::ProtocolStc8Hid())
                          : std::unique_ptr<stc::IStcProtocol>(new stc::ProtocolStc8Uart());
        if (name == "stc32")
            return useHid ? std::unique_ptr<stc::IStcProtocol>(new stc::ProtocolStc32Hid())
                          : std::unique_ptr<stc::IStcProtocol>(new stc::ProtocolStc32Uart());
        return nullptr;
    }

    // Clone the protocol implementation that identified itself during probing.
    inline std::unique_ptr<stc::IStcProtocol> CloneProtocolByName(const std::string& name,
        bool useHid)
    {
        if (auto p = MakeStc8FamilyProto(name, useHid)) return p;

        if (name == "stc89")       return std::make_unique<stc::ProtocolStc89>();
        if (name == "stc89a")      return std::make_unique<stc::ProtocolStc89A>();
        if (name == "stc12")       return std::make_unique<stc::ProtocolStc12>();
        if (name == "stc12a")      return std::make_unique<stc::ProtocolStc12A>();
        if (name == "stc12b")      return std::make_unique<stc::ProtocolStc12B>();
        if (name == "stc15")       return std::make_unique<stc::ProtocolStc15>();
        if (name == "stc15a")      return std::make_unique<stc::ProtocolStc15A>();
        return nullptr;
    }

    // Fallback when the chip name could not be resolved to a specific
    // implementation; depends on which families the transport can reach.
    inline std::unique_ptr<stc::IStcProtocol> MakeProtocol(int idx, bool useHid)
    {
        if (useHid) return std::unique_ptr<stc::IStcProtocol>(new stc::ProtocolStc32Hid());

        switch (idx) {
        case kProtoIdxStc89: return std::make_unique<stc::ProtocolStc89A>();
        case kProtoIdxStc12: return std::make_unique<stc::ProtocolStc12>();
        case kProtoIdxStc15: return std::make_unique<stc::ProtocolStc15>();
        case kProtoIdxStc8G: return std::unique_ptr<stc::IStcProtocol>(new stc::ProtocolStc8GUart());
        case kProtoIdxStc32: return std::unique_ptr<stc::IStcProtocol>(new stc::ProtocolStc32Uart());
        case kProtoIdxStc8:  return std::unique_ptr<stc::IStcProtocol>(new stc::ProtocolStc8Uart());
        default:             return std::unique_ptr<stc::IStcProtocol>(new stc::ProtocolStc32Uart());
        }
    }

    // Probe list used by the handshake step.
    //
    // Over serial every family is reachable: the classic parts answer to their
    // own init packets and STC8G/STC8 derive from the STC32 protocol, so all
    // ten variants are probed.
    //
    // Over HID only the parts with a USB interface are ever present in the
    // device list, and they are served by three implementations: ProtocolStc32
    // (STC32 / STC8H / 8051U), ProtocolStc8G (8G parts) and ProtocolStc8 (the
    // 08A variant of 8G). The remaining families cannot appear, so they are not
    // probed.
    inline std::vector<std::unique_ptr<stc::IStcProtocol>> MakeProbeList(bool useHid)
    {
        std::vector<std::unique_ptr<stc::IStcProtocol>> v;
        if (useHid) {
            // The Hid-suffixed classes know the status packet carries no 0x50
            // prefix over USB.
            v.push_back(std::unique_ptr<stc::IStcProtocol>(new stc::ProtocolStc32Hid()));
            v.push_back(std::unique_ptr<stc::IStcProtocol>(new stc::ProtocolStc8GHid()));
            v.push_back(std::unique_ptr<stc::IStcProtocol>(new stc::ProtocolStc8Hid()));
        }
        else {
            v.push_back(std::make_unique<stc::ProtocolStc89>());
            v.push_back(std::make_unique<stc::ProtocolStc89A>());
            v.push_back(std::make_unique<stc::ProtocolStc12>());
            v.push_back(std::make_unique<stc::ProtocolStc12A>());
            v.push_back(std::make_unique<stc::ProtocolStc12B>());
            v.push_back(std::make_unique<stc::ProtocolStc15A>());
            v.push_back(std::make_unique<stc::ProtocolStc15>());
            v.push_back(std::unique_ptr<stc::IStcProtocol>(new stc::ProtocolStc8GUart()));
            v.push_back(std::unique_ptr<stc::IStcProtocol>(new stc::ProtocolStc32Uart()));
            v.push_back(std::unique_ptr<stc::IStcProtocol>(new stc::ProtocolStc8Uart()));
        }
        return v;
    }

    // The 0x05 "prepare programming" step does not exist on the older
    // serial-only families.
    inline bool NeedsPrepare05(const std::string& protoName)
    {
        if (protoName == "stc89")  return false;
        if (protoName == "stc89a") return false;
        if (protoName == "stc12")  return false;
        if (protoName == "stc12a") return false;
        if (protoName == "stc12b") return false;
        return true;
    }

    // Legacy families whose BRT register cannot reach beyond 115200.
    inline bool IsLegacyBaudProto(const std::string& protoName)
    {
        return protoName == "stc89" || protoName == "stc89a" ||
            protoName == "stc12" || protoName == "stc12a" ||
            protoName == "stc12b";
    }

    // ========================================================================
    //  Flash sizing
    // ========================================================================

    inline uint32_t GetChipTotalFlash(const std::string& chipName, uint16_t magic)
    {
        if (chipName.find("12K128") != std::string::npos) return 131072;
        if (chipName.find("8K64") != std::string::npos) return 65536;
        if (chipName.find("8K60") != std::string::npos) return 65536;
        if (chipName.find("8K48") != std::string::npos) return 65536;
        if (chipName.find("8K32") != std::string::npos) return 32768;
        if (chipName.find("8K16") != std::string::npos) return 16384;
        if (chipName.find("4K64") != std::string::npos) return 65536;
        if (chipName.find("4K60") != std::string::npos) return 65536;
        if (chipName.find("4K48") != std::string::npos) return 65536;
        if (chipName.find("4K32") != std::string::npos) return 32768;
        if (chipName.find("4K16") != std::string::npos) return 16384;
        if (chipName.find("2K64") != std::string::npos) return 65536;
        if (chipName.find("2K60") != std::string::npos) return 65536;
        if (chipName.find("2K48") != std::string::npos) return 65536;
        if (chipName.find("2K32") != std::string::npos) return 32768;
        if (chipName.find("2K16") != std::string::npos) return 16384;
        if (chipName.find("1K33") != std::string::npos) return 33792;
        if (chipName.find("1K28") != std::string::npos) return 28672;
        if (chipName.find("1K17") != std::string::npos) return 17408;
        // 1K16 is the one entry the original table was missing. The part number
        // scheme is <series>K<size in KB>, so a 1K16 has 16 KB -- the same
        // silicon as an 8K16 with a different package. Without this line the
        // lookup falls through to the ID-based guess and reports 64 KB, which
        // would make the EEPROM split arithmetic wrong.
        if (chipName.find("1K16") != std::string::npos) return 16384;
        if (chipName.find("1K12") != std::string::npos) return 12288;
        if (chipName.find("1K08") != std::string::npos) return 12288;
        if (chipName.find("1K06") != std::string::npos) return 12288;
        if (chipName.find("1K04") != std::string::npos) return 12288;
        if (chipName.find("1K02") != std::string::npos) return 12288;

        uint8_t hi = (uint8_t)(magic >> 8);
        if (hi >= 0xfa) return 128 * 1024;
        return 64 * 1024;
    }

    // ========================================================================
    //  Device-list formatting
    // ========================================================================

    // The combo box stores "<friendly name> (<id>)" for both transports, where
    // <id> is a COM name for serial and a device interface path for HID. Pull
    // it back out; fall back to the raw text when no parentheses are present.
    inline std::string ExtractDeviceId(const std::string& display)
    {
        size_t lp = display.rfind('('), rp = display.rfind(')');
        if (lp != std::string::npos && rp != std::string::npos && rp > lp + 1)
            return display.substr(lp + 1, rp - lp - 1);
        return display;
    }

    inline std::string FormatHidDevice(const stcisp::HidDeviceInfo& d)
    {
        std::string label = d.displayName;
        if (label.empty()) label = "STC USB ISP";
        char ids[32];
        std::snprintf(ids, sizeof(ids), "%04X:%04X", d.vid, d.pid);
        return label + " [" + std::string(ids) + "] (" + d.devicePath + ")";
    }

} // namespace newisp
