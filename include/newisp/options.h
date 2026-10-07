#pragma once
// Command-line option parsing.

#include <string>
#include <vector>

#include "newisp/session.h"

namespace newisp {

    enum class Command {
        None,       // no command given; print usage
        List,       // list serial and HID devices
        Detect,     // probe and report the chip
        Burn,       // detect, erase, write, write options
        Help,
        Version,
    };

    struct CliOptions {
        Command     command = Command::None;
        BurnOptions burn{};

        std::string firmwarePath;
        bool        useHid = false;      // --hid / --serial
        bool        transportExplicit = false;

        // Raw values as given, before any family clamping.
        bool        overclock = false;
        bool        clockExternal = false;
        bool        clockExplicit = false;

        std::string familyName;   // --family stc32, etc.

        // Log verbosity, consumed by the logger in main.cpp.
        bool        quiet = false;
        bool        verbose = false;
    };

    // Parse argv into CliOptions. Returns 0 on success, non-zero when the
    // command line is malformed (in which case `error` explains why).
    int ParseCommandLine(int argc, char** argv, CliOptions& out,
        std::string& error);

    // The usage text, shared by --help and the malformed-input path.
    std::string UsageText(const char* programName);

} // namespace newisp
