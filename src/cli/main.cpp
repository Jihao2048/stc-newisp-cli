// newisp -- STC MCU programmer, cross-platform command line front end.
//
// Ported from the WinUI application of the same name. The protocol, chip tables
// and burn sequence are the same code, recompiled for Windows, macOS and Linux;
// what this file adds is argument handling, device selection and a logger that
// writes UTF-8 to a console.

#include "newisp/options.h"

#include "newisp/chip_logic.h"
#include "newisp/chip_table.h"
#include "newisp/hex_file.h"
#include "newisp/hid_channel.h"
#include "newisp/serial_channel.h"
#include "newisp/session.h"
#include "newisp/text.h"

#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace {

    std::atomic<bool> g_stopRequested{ false };

    void OnSignal(int)
    {
        g_stopRequested.store(true);
    }

    // ---------------------------------------------------------------------
    //  Console setup
    // ---------------------------------------------------------------------
    //
    // Windows consoles default to the OEM code page, which cannot represent the
    // Chinese log messages. Switching the output code page to UTF-8 is the same
    // thing the tool does for the file system paths below.
    void ConfigureConsole()
    {
#if defined(_WIN32)
        SetConsoleOutputCP(CP_UTF8);
        SetConsoleCP(CP_UTF8);

        // Enable ANSI escape processing so the progress line can overwrite
        // itself. Best effort: older consoles simply ignore the sequences.
        HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
        if (out != INVALID_HANDLE_VALUE) {
            DWORD mode = 0;
            if (GetConsoleMode(out, &mode)) {
                SetConsoleMode(out, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
            }
        }
#endif
    }

    bool StdoutIsTty()
    {
#if defined(_WIN32)
        HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
        if (out == INVALID_HANDLE_VALUE) return false;
        DWORD mode = 0;
        return GetConsoleMode(out, &mode) != 0;
#else
        return isatty(fileno(stdout)) != 0;
#endif
    }

#if defined(_WIN32)
    // Convert one UTF-16 command-line argument to UTF-8.
    //
    // This is the reason the Windows entry point is wmain rather than main:
    // argv from main() is encoded in the system ANSI code page, which cannot
    // represent a path such as 烧录测试文件, so a firmware path would arrive
    // already mangled and could not be printed correctly or passed on to the
    // wide-character file APIs. wmain hands us UTF-16, which is the real
    // representation Windows uses internally.
    std::string WideToUtf8(const wchar_t* w)
    {
        if (!w || !*w) return std::string();
        int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0,
            nullptr, nullptr);
        if (n <= 1) return std::string();
        std::string s((size_t)(n - 1), '\0');
        WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n, nullptr, nullptr);
        return s;
    }
#endif

    // ---------------------------------------------------------------------
    //  Logger
    // ---------------------------------------------------------------------
    //
    // The session layer emits two very different kinds of line: human progress
    // ("[4/7] 擦除成功") and raw packet dumps ("[发送] 0x03: ..."). Verbose mode
    // shows both, the default hides the dumps, and quiet shows only the output
    // of the final summary. Filtering by prefix is crude but keeps the session
    // code free of log levels, which is what the ported protocol layer expects.
    class Logger {
    public:
        Logger(bool quiet, bool verbose)
            : m_quiet(quiet), m_verbose(verbose)
        {
        }

        void operator()(const std::string& msg)
        {
            if (m_quiet && !IsImportant(msg)) return;

            // Packet dumps are noisy and only useful when debugging a
            // handshake, so they are opt-in.
            if (!m_verbose && IsPacketDump(msg)) return;

            std::cout << msg << '\n';
            std::cout.flush();
        }

    private:
        static bool IsPacketDump(const std::string& m)
        {
            return m.rfind("[发送]", 0) == 0 ||
                   m.rfind("[接收]", 0) == 0 ||
                   m.rfind("     ", 0) == 0;
        }

        static bool IsImportant(const std::string& m)
        {
            return m.rfind("[失败]", 0) == 0 ||
                   m.rfind("[提示]", 0) == 0 ||
                   m.rfind("检测成功", 0) == 0 ||
                   m.rfind("烧录完成", 0) == 0 ||
                   m.rfind("芯片型号", 0) == 0;
        }

        bool m_quiet = false;
        bool m_verbose = false;
    };

    // Progress goes to stderr so it cannot corrupt redirected stdout, and it
    // overwrites itself with a carriage return when attached to a terminal.
    //
    // Written through std::cerr for the same reason as the header lines: printf
    // would route the Chinese text through the Windows ANSI code page.
    class ProgressBar {
    public:
        explicit ProgressBar(bool enabled) : m_enabled(enabled) {}

        void operator()(int percent)
        {
            if (!m_enabled) return;
            if (percent == m_last) return;
            m_last = percent;

            // "\033[K" clears to end of line so a shorter value does not leave
            // digits behind from the previous one.
            std::cerr << "\r写入进度: " << std::setw(3) << percent << "%\033[K";
            std::cerr.flush();
            if (percent >= 100) std::cerr << '\n';
        }

    private:
        bool m_enabled = false;
        int  m_last = -1;
    };

    // ---------------------------------------------------------------------
    //  Device selection
    // ---------------------------------------------------------------------

    void PrintSerialPorts()
    {
        auto ports = stcisp::EnumerateSerialPorts();
        if (ports.empty()) {
            std::cout << "未发现串口设备\n";
            return;
        }
        std::cout << "串口设备 (" << ports.size() << "):\n";
        for (const auto& p : ports) {
            std::cout << "  " << p.device;
            if (!p.displayName.empty() && p.displayName != p.device)
                std::cout << "  -- " << p.displayName;
            std::cout << '\n';
        }
    }

    // PrintHidDevices lists what matters by default and everything on request.
    //
    // A typical desktop has a dozen or more HID devices -- mouse, touchpad,
    // keyboard, vendor utilities -- and listing them all buries the one board
    // the user is looking for. The default therefore shows only devices from
    // the STC vendor ID, which covers both the ISP interface (34BF:1001) and
    // the vendor's other HID functions, such as the USB-to-serial bridge
    // (34BF:FF09). Verbose mode still lists every HID device, which is what you
    // want when a board is not appearing where you expect and you need to see
    // whether it enumerated at all.
    void PrintHidDevices(bool verbose)
    {
        if (!stcisp::HidSupported()) {
            std::cout << "本版本未编译 USB HID 支持\n";
            return;
        }

        auto stc = stcisp::EnumerateHidDevices(stcisp::STC_HID_VID, 0);
        auto isp = stcisp::EnumerateHidDevices(stcisp::STC_HID_VID,
            stcisp::STC_HID_PID);

        std::cout << "STC ISP 设备 (VID " << newisp::Hex(stcisp::STC_HID_VID, 4)
                  << " PID " << newisp::Hex(stcisp::STC_HID_PID, 4) << "): "
                  << isp.size() << " 个\n";
        for (const auto& d : isp) {
            std::cout << "  " << newisp::FormatHidDevice(d) << '\n';
        }

        // Other devices from the same vendor, with the ISP interface already
        // accounted for above; these are usually the USB-serial bridges.
        std::vector<stcisp::HidDeviceInfo> other;
        for (const auto& d : stc) {
            if (d.pid == stcisp::STC_HID_PID) continue;
            other.push_back(d);
        }
        if (!other.empty()) {
            // These are the vendor's own HID devices other than the ISP
            // interface. The most common is the STC USB-to-serial bridge
            // (34BF:FF09), which despite the name "USB Serial" is a HID device
            // and not a USB CDC/ACM port: it does not appear in the serial
            // list above and cannot be opened as a COM port or a tty. It is
            // listed here so the device is accounted for rather than looking
            // like the tool missed it.
            std::cout << "\n其它 STC HID 设备 (VID "
                      << newisp::Hex(stcisp::STC_HID_VID, 4) << "，不含 ISP): "
                      << other.size() << " 个\n";
            for (const auto& d : other) {
                std::cout << "  " << newisp::FormatHidDevice(d) << '\n';
            }
        }

        if (!verbose) {
            if (isp.empty() && other.empty()) {
                std::cout << "\n未发现 STC 设备。若芯片已按 BOOT 上电，"
                             "可用 -v 列出全部 HID 设备排查。\n";
            }
            return;
        }

        // Verbose mode lists everything, which is what you want when a board is
        // not where you expect it: seeing that it enumerated under an
        // unexpected name is the whole point. The header repeats the count so
        // the list can be skipped at a glance.
        auto all = stcisp::EnumerateHidDevices(0, 0);
        std::cout << "\n全部 HID 设备: " << all.size()
                  << " 个（含鼠标、键盘等，仅供排查）\n";
        for (const auto& d : all) {
            std::cout << "  " << newisp::FormatHidDevice(d) << '\n';
        }
    }

    // Resolve which device to talk to.
    //
    // An explicit --device always wins. Otherwise, if exactly one candidate
    // exists, use it; if several do, refuse and list them, because picking one
    // at random can program the wrong board.
    bool ResolveDevice(newisp::BurnOptions& burn, bool useHid,
        std::string& error)
    {
        if (!burn.deviceId.empty()) return true;

        if (useHid) {
            if (!stcisp::HidSupported()) {
                error = "本版本未编译 USB HID 支持，请使用 --serial 或重新编译";
                return false;
            }
            auto devs = stcisp::EnumerateHidDevices(stcisp::STC_HID_VID,
                stcisp::STC_HID_PID);
            if (devs.empty()) {
                error = "未发现 STC USB ISP 设备（请按住 BOOT 键重新上电）";
                return false;
            }
            if (devs.size() > 1) {
                error = "发现多个 STC USB ISP 设备，请用 --device 指定：";
                for (const auto& d : devs)
                    error += "\n  " + newisp::FormatHidDevice(d);
                return false;
            }
            burn.deviceId = devs.front().devicePath;
            return true;
        }

        auto ports = stcisp::EnumerateSerialPorts();
        if (ports.empty()) {
            error = "未发现串口设备";
            return false;
        }
        if (ports.size() > 1) {
            error = "发现多个串口设备，请用 --device 指定：";
            for (const auto& p : ports) error += "\n  " + p.device;
            return false;
        }
        burn.deviceId = ports.front().device;
        return true;
    }

} // namespace

// The real entry point, shared by both platform wrappers below. It takes
// UTF-8 arguments on every platform, which is what the rest of the program
// assumes.
static int RunMain(const std::vector<std::string>& args)
{
    ConfigureConsole();

    newisp::CliOptions opts;
    std::string error;

    // ParseCommandLine wants a C-style argv; the strings themselves are what
    // matter and they are already UTF-8.
    std::vector<const char*> argv;
    argv.reserve(args.size() + 1);
    for (const auto& a : args) argv.push_back(a.c_str());
    argv.push_back(nullptr);

    const char* programName =
        args.empty() ? "newisp" : args.front().c_str();

    int rc = newisp::ParseCommandLine((int)args.size(),
        const_cast<char**>(argv.data()), opts, error);
    if (rc != 0) {
        std::cerr << "错误: " << error << "\n\n"
                  << newisp::UsageText(programName);
        return 2;
    }

    switch (opts.command) {
    case newisp::Command::None:
        // No arguments: this is someone who ran the tool to see what it is, so
        // show them what it can talk to and then how to use it. The device list
        // doubles as a check that the drivers are in place -- if a board is
        // plugged in and does not appear here, the problem is not this tool.
        PrintSerialPorts();
        std::cout << '\n';
        PrintHidDevices(opts.verbose);
        std::cout << '\n';
        std::cout << newisp::UsageText(programName);
        return 0;

    case newisp::Command::Help:
        std::cout << newisp::UsageText(programName);
        return 0;

    case newisp::Command::Version:
        std::cout << "newisp " << NEWISP_VERSION << '\n'
                  << "协议: STC89/90/10/11/12/15/8/8G/8H/32/8051U\n"
                  << "传输: 串口"
#if NEWISP_HAVE_HID
                  << " + USB HID"
#endif
                  << '\n';
        return 0;

    case newisp::Command::List:
        PrintSerialPorts();
        std::cout << '\n';
        PrintHidDevices(opts.verbose);
        return 0;

    case newisp::Command::Detect:
    case newisp::Command::Burn:
        break;
    }

    // ---- shared setup for detect and burn ----
    bool useHid = opts.burn.useHid;
    if (!ResolveDevice(opts.burn, useHid, error)) {
        std::cerr << "错误: " << error << '\n';
        return 1;
    }

    // A Ctrl-C during a long handshake should stop it cleanly rather than
    // leaving the port open.
    std::signal(SIGINT, OnSignal);
#if defined(SIGTERM)
    std::signal(SIGTERM, OnSignal);
#endif

    const bool tty = StdoutIsTty();
    Logger log(opts.quiet, opts.verbose || !tty);
    ProgressBar progress(tty && !opts.quiet);

    std::cout << "设备     : " << opts.burn.deviceId << '\n';
    std::cout << "传输方式 : " << (useHid ? "USB HID" : "串口") << '\n';

    if (opts.command == newisp::Command::Detect) {
        int r = newisp::RunDetect(opts.burn, log);
        return r;
    }

    // ---- burn ----
    stc::HexImage image;
    std::string hexError;
    if (!stc::LoadHexFile(opts.firmwarePath, image, hexError)) {
        std::cerr << "错误: " << hexError << '\n';
        return 1;
    }

    // Trim the image to the range that actually holds data: the parser fills
    // gaps with 0xFF and sizes the vector to the highest address, and writing
    // the trailing padding would waste flash cycles and time.
    std::vector<uint8_t> firmware(image.data.begin(),
        image.data.begin() + (ptrdiff_t)(image.maxAddr + 1));

    // Print through std::cout rather than printf: on Windows printf writes
    // through the ANSI code page, so a firmware path containing non-ASCII
    // characters (a Chinese directory name, say) comes out mangled. The stream
    // operators pass the UTF-8 bytes through unchanged, and the console code
    // page was already set to UTF-8 above.
    std::cout << "固件     : " << opts.firmwarePath
              << " (" << firmware.size() << " 字节, 地址 0x"
              << newisp::Hex(image.minAddr, 4) << "-0x"
              << newisp::Hex(image.maxAddr, 4) << ")\n";

    if (firmware.empty()) {
        std::cerr << "错误: 固件为空\n";
        return 1;
    }

    if (opts.burn.targetFreq)
        std::cout << "目标频率 : "
                  << newisp::FormatDouble(opts.burn.targetFreq / 1e6, 4)
                  << " MHz\n";
    if (!useHid)
        std::cout << "波特率   : " << opts.burn.baudRate << '\n';
    if (opts.burn.eepromBytes)
        std::cout << "EEPROM   : " << opts.burn.eepromBytes << " 字节\n";

    int r = newisp::RunBurn(opts.burn, firmware, log, progress);
    if (r == 0) {
        std::cout << "烧录成功\n";
    }
    else {
        std::cerr << "烧录失败\n";
    }
    return r;
}

#if defined(_WIN32)

// Windows entry point: UTF-16 arguments, converted to UTF-8 once at the top.
//
// Using wmain instead of main is what makes a firmware path such as
// C:\cmdisp\newisp\烧录测试文件\ai8051u.hex survive: main() would hand us the
// path in the system ANSI code page, which mangles those characters before the
// program can do anything about it.
int wmain(int argc, wchar_t** argv)
{
    std::vector<std::string> args;
    args.reserve((size_t)argc);
    for (int i = 0; i < argc; ++i) args.push_back(WideToUtf8(argv[i]));
    if (args.empty()) args.push_back("newisp");

    return RunMain(args);
}

#else

// POSIX entry point: argv is already whatever byte encoding the user's locale
// uses, and on every modern Linux and macOS system that is UTF-8.
int main(int argc, char** argv)
{
    std::vector<std::string> args;
    args.reserve((size_t)argc);
    for (int i = 0; i < argc; ++i) args.push_back(argv[i] ? argv[i] : "");
    if (args.empty()) args.push_back("newisp");

    return RunMain(args);
}

#endif
