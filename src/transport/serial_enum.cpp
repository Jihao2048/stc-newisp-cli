// Serial port enumeration, one implementation per platform.
//
// The channel implementations share termios between macOS and Linux, but
// enumeration differs on all three: Windows walks the SetupAPI port class,
// macOS asks IOKit for IOSerialBSDClient services, and Linux scans /dev plus
// sysfs. Keeping them in one file with a platform switch is clearer than three
// near-empty files.

#include "newisp/serial_channel.h"
#include "newisp/text.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <string>
#include <vector>

// ===========================================================================
//  Shared helpers
// ===========================================================================
namespace stcisp {
namespace {

    // Compare device names with embedded numbers ordered numerically, so COM9
    // precedes COM10 and ttyUSB2 precedes ttyUSB10. The CLI lists ports in this
    // order and users read it as a numbered list.
    bool DeviceLess(const std::string& x, const std::string& y)
    {
        size_t i = 0, j = 0;
        while (i < x.size() && j < y.size()) {
            const bool dx = std::isdigit((unsigned char)x[i]) != 0;
            const bool dy = std::isdigit((unsigned char)y[j]) != 0;
            if (dx && dy) {
                size_t si = i, sj = j;
                while (i < x.size() && std::isdigit((unsigned char)x[i])) ++i;
                while (j < y.size() && std::isdigit((unsigned char)y[j])) ++j;
                long nx = std::stol(x.substr(si, i - si));
                long ny = std::stol(y.substr(sj, j - sj));
                if (nx != ny) return nx < ny;
            }
            else {
                if (x[i] != y[j]) return x[i] < y[j];
                ++i; ++j;
            }
        }
        return x.size() < y.size();
    }

    void SortPorts(std::vector<SerialPortInfo>& ports)
    {
        std::sort(ports.begin(), ports.end(),
            [](const SerialPortInfo& a, const SerialPortInfo& b) {
                return DeviceLess(a.device, b.device);
            });
    }

} // namespace
} // namespace stcisp

// ===========================================================================
//  Windows
// ===========================================================================
#if defined(_WIN32)

#include <windows.h>
#include <setupapi.h>
#include <devguid.h>

namespace stcisp {
namespace {

    std::string ToUtf8(const wchar_t* w)
    {
        if (!w || !*w) return std::string();
        int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
        if (n <= 1) return std::string();
        std::string s((size_t)(n - 1), '\0');
        WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n, nullptr, nullptr);
        return s;
    }

    // Pull the "COM7" name out of a device instance ID / registry property.
    std::string ExtractComPort(HDEVINFO devInfo, SP_DEVINFO_DATA& devData)
    {
        // SPDRP_FRIENDLYNAME looks like "USB-SERIAL CH340 (COM7)"; the port name
        // is in the trailing parentheses.
        HKEY key = SetupDiOpenDevRegKey(devInfo, &devData, DICS_FLAG_GLOBAL,
            0, DIREG_DEV, KEY_READ);
        if (key == INVALID_HANDLE_VALUE) return std::string();

        wchar_t buf[256] = { 0 };
        DWORD size = sizeof(buf);
        DWORD type = 0;
        LONG rc = RegQueryValueExW(key, L"PortName", nullptr, &type,
            reinterpret_cast<LPBYTE>(buf), &size);
        RegCloseKey(key);

        if (rc != ERROR_SUCCESS || type != REG_SZ) return std::string();
        return ToUtf8(buf);
    }

    std::string FriendlyName(HDEVINFO devInfo, SP_DEVINFO_DATA& devData)
    {
        wchar_t buf[512] = { 0 };
        if (SetupDiGetDeviceRegistryPropertyW(devInfo, &devData,
            SPDRP_FRIENDLYNAME, nullptr,
            reinterpret_cast<PBYTE>(buf), sizeof(buf), nullptr)) {
            return ToUtf8(buf);
        }
        return std::string();
    }

} // namespace

    std::vector<SerialPortInfo> EnumerateSerialPorts()
    {
        std::vector<SerialPortInfo> result;

        HDEVINFO devInfo = SetupDiGetClassDevsW(&GUID_DEVCLASS_PORTS,
            nullptr, nullptr, DIGCF_PRESENT);
        if (devInfo == INVALID_HANDLE_VALUE) return result;

        for (DWORD i = 0;; ++i) {
            SP_DEVINFO_DATA devData = {};
            devData.cbSize = sizeof(devData);
            if (!SetupDiEnumDeviceInfo(devInfo, i, &devData)) break;

            std::string port = ExtractComPort(devInfo, devData);
            if (port.empty()) continue;

            SerialPortInfo info;
            info.device = port;
            info.displayName = FriendlyName(devInfo, devData);
            if (info.displayName.empty()) info.displayName = port;
            result.push_back(std::move(info));
        }

        SetupDiDestroyDeviceInfoList(devInfo);
        SortPorts(result);
        return result;
    }

} // namespace stcisp

// ===========================================================================
//  macOS
// ===========================================================================
#elif defined(__APPLE__)

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/serial/IOSerialKeys.h>

// The "main port" default was renamed in the macOS 12 SDK: what used to be
// kIOMasterPortDefault is now kIOMainPortDefault. Both refer to the same
// bootstrap port, and both names exist as deprecated aliases in the newer SDK,
// so pick whichever this SDK actually declares.
//
// This matters for older systems: a Mac VM is often a release or two behind,
// and using the new name against an old SDK is a compile error, while the old
// name against a new SDK is only a deprecation warning.
#if defined(__MAC_OS_X_VERSION_MAX_ALLOWED) && __MAC_OS_X_VERSION_MAX_ALLOWED < 120000
#define NEWISP_IO_MAIN_PORT kIOMasterPortDefault
#else
#define NEWISP_IO_MAIN_PORT kIOMainPortDefault
#endif

namespace stcisp {
namespace {

    std::string CfToUtf8(CFStringRef s)
    {
        if (!s) return std::string();
        CFIndex len = CFStringGetLength(s);
        CFIndex max = CFStringGetMaximumSizeForEncoding(len, kCFStringEncodingUTF8) + 1;
        std::string out((size_t)max, '\0');
        if (!CFStringGetCString(s, &out[0], max, kCFStringEncodingUTF8))
            return std::string();
        out.resize(std::strlen(out.c_str()));
        return out;
    }

} // namespace

    std::vector<SerialPortInfo> EnumerateSerialPorts()
    {
        std::vector<SerialPortInfo> result;

        // Every POSIX tty that speaks the serial protocol is published as an
        // IOSerialBSDClient service.
        CFMutableDictionaryRef match = IOServiceMatching(kIOSerialBSDServiceValue);
        if (!match) return result;
        CFDictionarySetValue(match, CFSTR(kIOSerialBSDTypeKey),
            CFSTR(kIOSerialBSDAllTypes));

        io_iterator_t it = IO_OBJECT_NULL;
        if (IOServiceGetMatchingServices(NEWISP_IO_MAIN_PORT, match, &it) != KERN_SUCCESS)
            return result;

        io_object_t service;
        while ((service = IOIteratorNext(it)) != IO_OBJECT_NULL) {
            // Prefer the stable /dev/cu.* (callout) path over /dev/tty.*:
            // opening tty.* blocks waiting for DCD, which a self-powered board
            // never asserts.
            CFTypeRef path = IORegistryEntryCreateCFProperty(service,
                CFSTR(kIOCalloutDeviceKey), kCFAllocatorDefault, 0);
            if (path) {
                SerialPortInfo info;
                info.device = CfToUtf8(static_cast<CFStringRef>(path));

                CFTypeRef name = IORegistryEntryCreateCFProperty(service,
                    CFSTR(kIOTTYDeviceKey), kCFAllocatorDefault, 0);
                info.displayName = name ? CfToUtf8(static_cast<CFStringRef>(name))
                                        : info.device;
                if (name) CFRelease(name);
                CFRelease(path);

                if (!info.device.empty()) result.push_back(std::move(info));
            }
            IOObjectRelease(service);
        }
        IOObjectRelease(it);

        SortPorts(result);
        return result;
    }

} // namespace stcisp

// ===========================================================================
//  Linux and other POSIX systems
// ===========================================================================
#else

#include <dirent.h>
#include <sys/stat.h>

namespace stcisp {
namespace {

    // Candidate device-name prefixes, in the order the vendor tools list them.
    // Only these are reported: scanning all of /dev would bury the real ports
    // under hundreds of unrelated entries.
    const char* const kPortPrefixes[] = {
        "ttyUSB",   // USB-serial bridges: CH340, CP2102, FT232, PL2303
        "ttyACM",   // USB CDC-ACM (some STC boards and debug probes)
        "ttyAMA",   // Raspberry Pi / SoC UART
        "ttyS",     // legacy 8250-style UART
        "ttyPS",    // Xilinx
        "ttySC",    // Renesas
        "rfcomm",   // Bluetooth serial
    };

    bool IsCandidate(const std::string& name)
    {
        for (const char* p : kPortPrefixes) {
            size_t n = std::strlen(p);
            if (name.compare(0, n, p) != 0) continue;
            // Require at least one digit after the prefix, so "ttyS" alone is
            // rejected but "ttyS0" is accepted.
            if (name.size() == n) return false;
            return std::isdigit((unsigned char)name[n]) != 0;
        }
        return false;
    }

    // /dev/ttyS0..ttyS31 mostly do not exist as real hardware, but the device
    // nodes are present on every PC. Reading the sysfs type tells us whether
    // the kernel bound a driver to it.
    bool IsUsableTty(const std::string& path, const std::string& name)
    {
        // Prefer the sysfs view when it exists.
        std::string sys = "/sys/class/tty/" + name + "/device";
        struct stat st;
        if (::stat(sys.c_str(), &st) == 0) return true;

        // No sysfs entry: fall back to "the node exists and is a character
        // device". This keeps the list working in containers and on systems
        // without sysfs mounted.
        if (::stat(path.c_str(), &st) != 0) return false;
        return S_ISCHR(st.st_mode);
    }

} // namespace

    std::vector<SerialPortInfo> EnumerateSerialPorts()
    {
        std::vector<SerialPortInfo> result;

        DIR* dir = ::opendir("/dev");
        if (!dir) return result;

        while (dirent* e = ::readdir(dir)) {
            std::string name = e->d_name;
            if (!IsCandidate(name)) continue;

            std::string path = "/dev/" + name;
            if (!IsUsableTty(path, name)) continue;

            SerialPortInfo info;
            info.device = path;
            info.displayName = name;
            result.push_back(std::move(info));
        }
        ::closedir(dir);

        SortPorts(result);
        return result;
    }

} // namespace stcisp

#endif
