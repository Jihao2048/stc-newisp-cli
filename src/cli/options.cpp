// Command-line parsing.
//
// Hand-written rather than pulled from a library: the option set is small, the
// tool has to build with nothing but a C++17 compiler, and a dependency here
// would be the largest thing in the tree.

#include "newisp/options.h"

#include "newisp/chip_logic.h"

#include <cstdlib>
#include <cstring>

namespace newisp {

    namespace {

        // Accept "115200", "115.2k" and "1M" for baud rates and similar
        // quantities, because those are how the numbers appear in datasheets.
        bool ParseRate(const char* s, uint32_t& out)
        {
            if (!s || !*s) return false;
            char* end = nullptr;
            double v = std::strtod(s, &end);
            if (end == s) return false;
            if (*end == 'k' || *end == 'K') { v *= 1000.0; ++end; }
            else if (*end == 'm' || *end == 'M') { v *= 1000000.0; ++end; }
            if (*end != '\0') return false;
            if (v <= 0) return false;
            out = (uint32_t)(v + 0.5);
            return true;
        }

        // --eeprom 4K / --eeprom 4096 / --eeprom 0
        bool ParseBytes(const char* s, uint32_t& out)
        {
            if (!s || !*s) return false;
            char* end = nullptr;
            double v = std::strtod(s, &end);
            if (end == s) return false;
            if (*end == 'k' || *end == 'K') { v *= 1024.0; ++end; }
            else if (*end == 'm' || *end == 'M') { v *= 1024.0 * 1024.0; ++end; }
            if (*end != '\0') return false;
            if (v < 0) return false;
            out = (uint32_t)(v + 0.5);
            return true;
        }

        // Map a --family name onto the menu index used as a probe fallback.
        int FamilyToIndex(const std::string& name)
        {
            if (name == "stc89" || name == "stc90") return kProtoIdxStc89;
            if (name == "stc12" || name == "stc10" || name == "stc11")
                return kProtoIdxStc12;
            if (name == "stc15") return kProtoIdxStc15;
            if (name == "stc8g") return kProtoIdxStc8G;
            if (name == "stc32" || name == "stc8h" || name == "8051u")
                return kProtoIdxStc32;
            if (name == "stc8") return kProtoIdxStc8;
            return -1;
        }

        // Frequencies above the vendor's validated ceiling are no longer
        // reachable from the command line; see the note where the overclock
        // table used to live in chip_logic.h. Anything above 45 MHz is still
        // rejected explicitly so a stale command line fails loudly instead of
        // being silently clamped.
        bool IsAboveValidatedRange(uint32_t freq)
        {
            return freq > kFreqTable[kNumFreq - 1];
        }

    } // namespace

    std::string UsageText(const char* programName)
    {
        // Show only the file name, not the path it was invoked by.
        //
        // argv[0] is whatever the shell was given, which is frequently an
        // absolute path: "C:\Users\...\Release\newisp.exe" is 50 characters of
        // noise that pushes every description off the right edge of the block.
        // The user already knows where their binary lives, and the examples
        // read as commands they can type. Both separators are handled so the
        // same code is correct on Windows and POSIX.
        std::string p = programName ? programName : "newisp";
        size_t slash = p.find_last_of("/\\");
        if (slash != std::string::npos && slash + 1 < p.size())
            p = p.substr(slash + 1);
        if (p.empty()) p = "newisp";

        // Pad the command column rather than hard-coding a run of spaces, so
        // the block still lines up if the executable is renamed.
        //
        // The width has to clear the longest invocation, which is
        // "<name> burn -f FILE [options] [device]" -- about 46 characters for
        // "newisp.exe". A narrower column silently collapses the last two rows
        // into one run of text, which is what it did at 40.
        const size_t cmdWidth = 48;
        auto usage = [&](const std::string& syntax,
                         const std::string& description) {
            std::string line = "  " + p + " " + syntax;
            if (line.size() < cmdWidth) line.append(cmdWidth - line.size(), ' ');
            else line += ' ';
            return line + description + "\n";
        };

        return
            "newisp " + std::string(NEWISP_VERSION) +
            " -- STC MCU programmer (serial + USB HID)\n"
            "\n"
            "Usage:\n" +
            usage("", "list devices, then this help") +
            usage("list", "list serial and HID devices") +
            usage("detect [transport] [device]", "probe and identify a chip") +
            usage("burn -f FILE [options] [device]", "erase, write and set options") +
            "\n"
            "Transport:\n"
            "  -s, --serial              use the serial ISP monitor (default)\n"
            "  -H, --hid                 use the factory USB HID ISP interface\n"
            "\n"
            "Device:\n"
            "  -d, --device NAME         COM7, /dev/ttyUSB0, or a HID interface path\n"
            "                            (default: the only device found, or a\n"
            "                            friendly error naming the candidates)\n"
            "\n"
            "Programming:\n"
            "  -f, --file FILE           Intel HEX firmware image (burn only)\n"
            "  -b, --baud RATE           serial baud rate (default 115200)\n"
            "                            accepts 115200, 115.2k, 1M\n"
            "  -F, --freq HZ             target IRC frequency (default 24 MHz)\n"
            "                            accepts 24000000, 24M, 11.0592M;\n"
            "                            the maximum is 45 MHz\n"
            "  -e, --eeprom SIZE         EEPROM split, e.g. 4K (0 = keep chip setting)\n"
            "      --clock internal|external\n"
            "                            clock source for STC89/12/15 option bytes\n"
            "      --family NAME         fallback family when probing identifies\n"
            "                            nothing: stc89, stc12, stc15, stc8g,\n"
            "                            stc32, stc8\n"
            "      --force-serial        try serial even for a part the manual\n"
            "                            calls HID-only (8051U). The probe\n"
            "                            decides whether it works\n"
            "\n"
            "Output:\n"
            "  -q, --quiet               only errors and the final summary\n"
            "  -v, --verbose             include the raw packet dumps\n"
            "\n"
            "General:\n"
            "  -h, --help                show this help\n"
            "  -V, --version             show the version\n"
            "\n"
            "Examples:\n"
            "  " + p + " list\n"
            "  " + p + " detect --device /dev/ttyUSB0\n"
            "  " + p + " burn -f app.hex -d COM7 -F 35M -e 4K\n"
            "  " + p + " burn -f app.hex --hid        # hold BOOT while plugging in\n"
            "\n"
            "Notes:\n"
            "  Serial: the tool pulses DTR and spams the wakeup byte, so it can\n"
            "  catch a chip that is still running user code. If the board has no\n"
            "  auto-reset circuit, power-cycle it during the handshake.\n"
            "\n"
            "  HID: hold the BOOT pin low while plugging the board in. There is\n"
            "  no auto-reset over USB.\n"
            "\n"
            "  The 8051U family has no serial ISP monitor at all and can only be\n"
            "  programmed over HID.\n";
    }

    int ParseCommandLine(int argc, char** argv, CliOptions& out,
        std::string& error)
    {
        if (argc < 2) {
            out.command = Command::None;
            return 0;
        }

        // ---- subcommand ----
        std::string cmd = argv[1];
        int i = 2;

        bool quiet = false;
        bool verbose = false;

        if (cmd == "-h" || cmd == "--help" || cmd == "help") {
            out.command = Command::Help;
            return 0;
        }
        if (cmd == "-V" || cmd == "--version" || cmd == "version") {
            out.command = Command::Version;
            return 0;
        }
        if (cmd == "list")        out.command = Command::List;
        else if (cmd == "detect") out.command = Command::Detect;
        else if (cmd == "burn")   out.command = Command::Burn;
        else if (cmd == "-v" || cmd == "--verbose" || cmd == "-q" ||
                 cmd == "--quiet") {
            // Only a verbosity flag was given. Treat it as the no-command case
            // so the tool prints the device list and usage, with the flag still
            // honoured; this is what someone types when they want to see every
            // HID device without first knowing that "list" exists.
            out.command = Command::None;
            i = 1;   // re-scan from the flag itself
        }
        else {
            error = "unknown command: " + cmd;
            return 2;
        }

        // ---- options ----
        for (; i < argc; ++i) {
            std::string a = argv[i];

            auto needValue = [&](const char* what) -> const char* {
                if (i + 1 >= argc) {
                    error = std::string("option ") + what + " needs a value";
                    return nullptr;
                }
                return argv[++i];
            };

            if (a == "-h" || a == "--help") {
                out.command = Command::Help;
                return 0;
            }
            else if (a == "-V" || a == "--version") {
                out.command = Command::Version;
                return 0;
            }
            else if (a == "-s" || a == "--serial") {
                out.useHid = false;
                out.transportExplicit = true;
            }
            else if (a == "-H" || a == "--hid") {
                out.useHid = true;
                out.transportExplicit = true;
            }
            else if (a == "-q" || a == "--quiet") {
                quiet = true;
            }
            else if (a == "-v" || a == "--verbose") {
                verbose = true;
            }
            else if (a == "-f" || a == "--file") {
                const char* v = needValue("--file");
                if (!v) return 2;
                out.firmwarePath = v;
            }
            else if (a == "-d" || a == "--device") {
                const char* v = needValue("--device");
                if (!v) return 2;
                out.burn.deviceId = v;
            }
            else if (a == "-b" || a == "--baud") {
                const char* v = needValue("--baud");
                if (!v) return 2;
                if (!ParseRate(v, out.burn.baudRate)) {
                    error = std::string("cannot parse baud rate: ") + v;
                    return 2;
                }
            }
            else if (a == "-F" || a == "--freq") {
                const char* v = needValue("--freq");
                if (!v) return 2;
                if (!ParseRate(v, out.burn.targetFreq)) {
                    error = std::string("cannot parse frequency: ") + v;
                    return 2;
                }
            }
            else if (a == "-e" || a == "--eeprom") {
                const char* v = needValue("--eeprom");
                if (!v) return 2;
                if (!ParseBytes(v, out.burn.eepromBytes)) {
                    error = std::string("cannot parse EEPROM size: ") + v;
                    return 2;
                }
            }
            else if (a == "--clock") {
                const char* v = needValue("--clock");
                if (!v) return 2;
                std::string s = v;
                if (s == "external" || s == "ext" || s == "crystal") {
                    out.clockExternal = true;
                }
                else if (s == "internal" || s == "int" || s == "irc") {
                    out.clockExternal = false;
                }
                else {
                    error = std::string("--clock expects internal or external, got: ") + s;
                    return 2;
                }
                out.clockExplicit = true;
            }
            else if (a == "--family") {
                const char* v = needValue("--family");
                if (!v) return 2;
                out.familyName = v;
                int idx = FamilyToIndex(out.familyName);
                if (idx < 0) {
                    error = std::string("unknown family: ") + out.familyName;
                    return 2;
                }
                out.burn.protoOverride = idx;
            }
            else if (a == "--force-serial") {
                // Allow a chip that the documentation marks as HID-only to be
                // driven over serial anyway. The probe decides: if the chip
                // answers, the transfer can proceed.
                out.burn.allowHidOnlySerial = true;
            }
            else if (!a.empty() && a[0] == '-') {
                error = "unknown option: " + a;
                return 2;
            }
            else {
                // A bare argument is the device name, so the common case reads
                // naturally: newisp detect /dev/ttyUSB0
                if (out.burn.deviceId.empty()) {
                    out.burn.deviceId = a;
                }
                else {
                    error = "unexpected extra argument: " + a;
                    return 2;
                }
            }
        }

        // ---- consistency ----
        if (out.command == Command::Burn && out.firmwarePath.empty()) {
            error = "burn requires a firmware image (-f FILE)";
            return 2;
        }

        // The overclock band is gone, so a frequency above the vendor's ceiling
        // is a mistake worth reporting rather than a request to be honoured or
        // quietly clamped. Silence here would leave the user believing they had
        // asked for something the tool never attempted.
        if (out.burn.targetFreq && IsAboveValidatedRange(out.burn.targetFreq)) {
            error = "target frequency " + std::to_string(out.burn.targetFreq) +
                " Hz is above the validated maximum (" +
                std::to_string(kFreqTable[kNumFreq - 1] / 1000000) +
                " MHz) and the overclock band has been removed";
            return 2;
        }

        out.burn.clockExternal = out.clockExternal;
        out.burn.useHid = out.useHid;

        out.quiet = quiet;
        out.verbose = verbose;

        return 0;
    }

} // namespace newisp
