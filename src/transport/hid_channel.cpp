// USB HID transport.
//
// Two backends sit behind the same interface, chosen at compile time:
//
//   Windows   the system HID class driver through hid.dll and SetupAPI. These
//             DLLs ship with the operating system, so the Windows build has no
//             external dependency at all -- which matters because vcpkg needs
//             network access that is not always available.
//
//   macOS/Linux  hidapi, which wraps IOKit and hidraw respectively. Both
//             platforms have the library packaged, so it costs nothing to use.
//
// The behaviour that matters is identical on all three:
//
//   * The ISP interface uses 64-byte reports with report ID 0. The first byte
//     of every transfer is reserved for the report ID (0x00 for an unnumbered
//     interface) and is skipped on read, so the usable payload is one byte
//     shorter than the report.
//   * Short payloads are zero padded and long payloads are split across
//     reports, matching the reference tool's paddata(..., 64, 0x00).
//   * A read with no report pending must not block forever.
//   * There is no line coding and no reset line, so baud rate, parity, DTR and
//     RTS are accepted and ignored -- but they report success, because a caller
//     treating a normal HID session as a failure would abort the burn.

#include "newisp/hid_channel.h"
#include "newisp/text.h"

#include <algorithm>
#include <cstring>

#if defined(_WIN32)

#include <windows.h>
#include <setupapi.h>
#include <hidsdi.h>
#include <hidpi.h>

// hidclass.h declares GUID_DEVINTERFACE_HID with DEFINE_GUID, which expands to
// an extern declaration unless INITGUID is defined somewhere in the program.
// Relying on the import library is fragile, so define the GUID locally under a
// different name to avoid clashing with the SDK's symbol.
// {4D1E55B2-F16F-11CF-88CB-001111000030}
static const GUID NEWISP_GUID_DEVINTERFACE_HID =
{ 0x4D1E55B2, 0xF16F, 0x11CF, { 0x88, 0xCB, 0x00, 0x11, 0x11, 0x00, 0x00, 0x30 } };

#else

#if NEWISP_HAVE_HID
#include <hidapi/hidapi.h>
#endif

#endif

namespace stcisp {

#if defined(_WIN32)

    // =======================================================================
    //  Windows backend
    // =======================================================================
    namespace {

        std::string ToUtf8(const wchar_t* w)
        {
            if (!w || !*w) return std::string();
            int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0,
                nullptr, nullptr);
            if (n <= 1) return std::string();
            std::string s((size_t)(n - 1), '\0');
            WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n, nullptr, nullptr);
            return s;
        }

        std::wstring ToWide(const std::string& s)
        {
            if (s.empty()) return std::wstring();
            int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(),
                nullptr, 0);
            std::wstring w((size_t)n, L'\0');
            MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
            return w;
        }

        struct HandleGuard {
            HDEVINFO h = INVALID_HANDLE_VALUE;
            ~HandleGuard() {
                if (h != INVALID_HANDLE_VALUE) SetupDiDestroyDeviceInfoList(h);
            }
        };

        // Fetch the product string, falling back to the manufacturer string.
        std::string ReadProductString(HANDLE dev)
        {
            wchar_t buf[256] = { 0 };
            if (HidD_GetProductString(dev, buf, sizeof(buf)) && buf[0] != L'\0')
                return ToUtf8(buf);
            if (HidD_GetManufacturerString(dev, buf, sizeof(buf)) && buf[0] != L'\0')
                return ToUtf8(buf);
            return std::string();
        }

        // Determine how many bytes a report occupies on the wire.
        //
        // HidP_GetCaps reports byte lengths that include a leading report-ID
        // byte when the descriptor declares report IDs. For an unnumbered
        // interface the first byte of every transfer is still reserved by the
        // HID stack, so callers of WriteFile/ReadFile must always supply it
        // (0x00 when the device is unnumbered). We therefore always treat the
        // first byte as the report ID and always skip it on read.
        bool QueryReportSizes(HANDLE dev, size_t& reportSize)
        {
            PHIDP_PREPARSED_DATA ppd = nullptr;
            if (!HidD_GetPreparsedData(dev, &ppd)) return false;

            HIDP_CAPS caps = {};
            NTSTATUS st = HidP_GetCaps(ppd, &caps);
            HidD_FreePreparsedData(ppd);
            if (st != HIDP_STATUS_SUCCESS) return false;

            size_t outLen = (size_t)caps.OutputReportByteLength;
            if (outLen == 0) outLen = STC_HID_REPORT_SIZE;
            reportSize = outLen;
            return true;
        }

    } // namespace

    bool HidSupported() { return true; }

    std::vector<HidDeviceInfo> EnumerateHidDevices(uint16_t vid, uint16_t pid)
    {
        std::vector<HidDeviceInfo> result;

        HandleGuard guard;
        guard.h = SetupDiGetClassDevsW(&NEWISP_GUID_DEVINTERFACE_HID, nullptr,
            nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
        if (guard.h == INVALID_HANDLE_VALUE) return result;

        for (DWORD index = 0;; ++index) {
            SP_DEVICE_INTERFACE_DATA ifData = {};
            ifData.cbSize = sizeof(ifData);
            if (!SetupDiEnumDeviceInterfaces(guard.h, nullptr,
                &NEWISP_GUID_DEVINTERFACE_HID, index, &ifData)) {
                break;
            }

            DWORD needed = 0;
            SetupDiGetDeviceInterfaceDetailW(guard.h, &ifData, nullptr, 0,
                &needed, nullptr);
            if (needed == 0) continue;

            std::vector<BYTE> buffer(needed);
            auto* detail =
                reinterpret_cast<PSP_DEVICE_INTERFACE_DETAIL_DATA_W>(buffer.data());
            detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
            if (!SetupDiGetDeviceInterfaceDetailW(guard.h, &ifData, detail,
                needed, &needed, nullptr)) {
                continue;
            }

            std::string path = ToUtf8(detail->DevicePath);

            HANDLE dev = CreateFileW(detail->DevicePath,
                GENERIC_READ | GENERIC_WRITE,
                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                0, nullptr);
            if (dev == INVALID_HANDLE_VALUE) continue;

            HIDD_ATTRIBUTES attrs = {};
            attrs.Size = sizeof(attrs);
            bool ok = HidD_GetAttributes(dev, &attrs) != FALSE;

            HidDeviceInfo info;
            info.devicePath = path;
            if (ok) {
                info.vid = attrs.VendorID;
                info.pid = attrs.ProductID;
                info.displayName = ReadProductString(dev);
            }
            CloseHandle(dev);

            if (!ok) continue;
            if (vid != 0 && info.vid != vid) continue;
            if (pid != 0 && info.pid != pid) continue;

            result.push_back(std::move(info));
        }

        return result;
    }

    struct HidChannel::Impl {
        HANDLE handle = INVALID_HANDLE_VALUE;
    };

    HidChannel::HidChannel() : m_impl(new Impl()) {}
    HidChannel::~HidChannel() { Close(); }

    bool HidChannel::Open(const std::string& portName, uint32_t baudRate,
        bool /*evenParity*/)
    {
        Close();
        if (portName.empty()) return false;

        m_currentBaud = baudRate;

        // FILE_FLAG_OVERLAPPED is required so reads can be given a timeout; a
        // blocking handle would hang forever when no report is pending.
        std::wstring widePath = ToWide(portName);
        HANDLE h = CreateFileW(widePath.c_str(),
            GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
        if (h == INVALID_HANDLE_VALUE) return false;

        size_t reportSize = STC_HID_REPORT_SIZE;
        if (!QueryReportSizes(h, reportSize)) {
            // Some ISP descriptors are minimal; fall back to the documented
            // 64-byte report rather than refusing to open.
            reportSize = STC_HID_REPORT_SIZE;
        }

        m_impl->handle = h;
        m_reportSize = reportSize;
        m_inputReportSize = reportSize;
        m_portName = portName;

        Flush();
        return true;
    }

    void HidChannel::Close()
    {
        if (m_impl && m_impl->handle != INVALID_HANDLE_VALUE) {
            CloseHandle(m_impl->handle);
            m_impl->handle = INVALID_HANDLE_VALUE;
        }
        m_currentBaud = 0;
    }

    bool HidChannel::IsOpen() const
    {
        return m_impl && m_impl->handle != INVALID_HANDLE_VALUE;
    }

    bool HidChannel::SetBaudRate(uint32_t baudRate)
    {
        if (!IsOpen()) return false;
        m_currentBaud = baudRate;
        return true;
    }

    bool HidChannel::SetParity(bool /*evenParity*/) { return IsOpen(); }
    bool HidChannel::SetDtr(bool /*asserted*/) { return IsOpen(); }
    bool HidChannel::SetRts(bool /*asserted*/) { return IsOpen(); }

    bool HidChannel::PulseReset(int /*pin*/, uint32_t /*holdMs*/,
        uint32_t /*releaseMs*/)
    {
        // Nothing to pulse: entering the ISP monitor requires a power cycle
        // with the BOOT pin held low, which cannot be driven over USB.
        return IsOpen();
    }

    namespace {

        // Write one report, waiting for the overlapped operation to finish.
        bool WriteReportBlocking(HANDLE h, const uint8_t* report, size_t len)
        {
            OVERLAPPED ov = {};
            ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            if (!ov.hEvent) return false;

            DWORD written = 0;
            BOOL ok = WriteFile(h, report, (DWORD)len, &written, &ov);
            bool success = false;

            if (ok) {
                success = (written == len);
            }
            else if (GetLastError() == ERROR_IO_PENDING) {
                if (WaitForSingleObject(ov.hEvent, 2000) == WAIT_OBJECT_0) {
                    if (GetOverlappedResult(h, &ov, &written, FALSE))
                        success = (written == len);
                }
                else {
                    CancelIoEx(h, &ov);
                    GetOverlappedResult(h, &ov, &written, TRUE);
                }
            }

            CloseHandle(ov.hEvent);
            return success;
        }

    } // namespace

    bool HidChannel::Write(const std::vector<uint8_t>& data)
    {
        if (!IsOpen()) return false;

        const size_t reportSize = m_reportSize ? m_reportSize : STC_HID_REPORT_SIZE;
        if (reportSize < 2) return false;
        const size_t payloadSize = reportSize - 1;

        size_t offset = 0;
        do {
            std::vector<uint8_t> report(reportSize, 0x00);   // report ID 0
            size_t chunk = data.size() - offset;
            if (chunk > payloadSize) chunk = payloadSize;
            if (chunk > 0) {
                std::copy(data.begin() + (ptrdiff_t)offset,
                    data.begin() + (ptrdiff_t)(offset + chunk),
                    report.begin() + 1);
            }
            if (!WriteReportBlocking(m_impl->handle, report.data(), report.size()))
                return false;
            offset += chunk;
        } while (offset < data.size());

        return true;
    }

    std::vector<uint8_t> HidChannel::Read(size_t maxBytes, uint32_t timeoutMs)
    {
        std::vector<uint8_t> result;
        if (!IsOpen()) return result;

        const size_t reportSize =
            m_inputReportSize ? m_inputReportSize : STC_HID_REPORT_SIZE;
        std::vector<uint8_t> buffer(reportSize, 0x00);

        OVERLAPPED ov = {};
        ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!ov.hEvent) return result;

        HANDLE h = m_impl->handle;
        DWORD read = 0;
        BOOL ok = ReadFile(h, buffer.data(), (DWORD)buffer.size(), &read, &ov);
        bool success = false;

        if (ok) {
            success = (read > 0);
        }
        else if (GetLastError() == ERROR_IO_PENDING) {
            DWORD wait = WaitForSingleObject(ov.hEvent, timeoutMs);
            if (wait == WAIT_OBJECT_0) {
                if (GetOverlappedResult(h, &ov, &read, FALSE) && read > 0)
                    success = true;
            }
            else {
                // Timed out. Cancel the outstanding request and reap it so the
                // buffer is not touched after we return.
                CancelIoEx(h, &ov);
                GetOverlappedResult(h, &ov, &read, TRUE);
            }
        }

        CloseHandle(ov.hEvent);

        if (!success) return result;

        // Drop the leading report-ID byte.
        if (read <= 1) return result;

        size_t payloadLen = (size_t)read - 1;
        if (payloadLen > maxBytes) payloadLen = maxBytes;
        result.assign(buffer.begin() + 1,
            buffer.begin() + 1 + (ptrdiff_t)payloadLen);
        return result;
    }

    void HidChannel::Flush()
    {
        if (!IsOpen()) return;

        const size_t reportSize =
            m_inputReportSize ? m_inputReportSize : STC_HID_REPORT_SIZE;
        std::vector<uint8_t> buffer(reportSize, 0x00);

        // Drain pending input reports without blocking. Each attempt costs a
        // cancelled I/O request, so bound the loop.
        for (int i = 0; i < 64; ++i) {
            OVERLAPPED ov = {};
            ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            if (!ov.hEvent) break;

            DWORD read = 0;
            BOOL ok = ReadFile(m_impl->handle, buffer.data(),
                (DWORD)buffer.size(), &read, &ov);
            bool got = false;

            if (ok) {
                got = (read > 0);
            }
            else if (GetLastError() == ERROR_IO_PENDING) {
                if (WaitForSingleObject(ov.hEvent, 0) == WAIT_OBJECT_0) {
                    if (GetOverlappedResult(m_impl->handle, &ov, &read, FALSE) &&
                        read > 0) {
                        got = true;
                    }
                }
                else {
                    CancelIoEx(m_impl->handle, &ov);
                    GetOverlappedResult(m_impl->handle, &ov, &read, TRUE);
                }
            }

            CloseHandle(ov.hEvent);
            if (!got) break;
        }
    }

#else  // !_WIN32

    // =======================================================================
    //  macOS / Linux backend (hidapi)
    // =======================================================================
    namespace {

        // hidapi reports product and manufacturer strings as wchar_t. The width
        // is 4 bytes on Linux and macOS, so encode code points by hand; a full
        // conversion library would be overkill for a product string.
        std::string WideToUtf8(const wchar_t* w)
        {
            if (!w || !*w) return std::string();

            std::string out;
            for (const wchar_t* p = w; *p; ++p) {
                uint32_t cp = (uint32_t)*p;
                if (cp < 0x80) {
                    out += (char)cp;
                }
                else if (cp < 0x800) {
                    out += (char)(0xC0 | (cp >> 6));
                    out += (char)(0x80 | (cp & 0x3F));
                }
                else if (cp < 0x10000) {
                    out += (char)(0xE0 | (cp >> 12));
                    out += (char)(0x80 | ((cp >> 6) & 0x3F));
                    out += (char)(0x80 | (cp & 0x3F));
                }
                else {
                    out += (char)(0xF0 | (cp >> 18));
                    out += (char)(0x80 | ((cp >> 12) & 0x3F));
                    out += (char)(0x80 | ((cp >> 6) & 0x3F));
                    out += (char)(0x80 | (cp & 0x3F));
                }
            }
            return out;
        }

    } // namespace

    bool HidSupported()
    {
#if NEWISP_HAVE_HID
        return true;
#else
        return false;
#endif
    }

    std::vector<HidDeviceInfo> EnumerateHidDevices(uint16_t vid, uint16_t pid)
    {
        std::vector<HidDeviceInfo> result;

#if NEWISP_HAVE_HID
        if (hid_init() != 0) return result;

        // A wildcard is expressed as 0x0000 to hidapi.
        hid_device_info* list = hid_enumerate(vid, pid);
        for (hid_device_info* d = list; d != nullptr; d = d->next) {
            HidDeviceInfo info;
            // The path is narrow UTF-8 on Linux and macOS.
            info.devicePath = d->path ? d->path : "";
            info.vid = d->vendor_id;
            info.pid = d->product_id;

            info.displayName = WideToUtf8(d->product_string);
            if (info.displayName.empty())
                info.displayName = WideToUtf8(d->manufacturer_string);

            if (!info.devicePath.empty()) result.push_back(std::move(info));
        }
        hid_free_enumeration(list);
#else
        (void)vid; (void)pid;
#endif

        return result;
    }

    struct HidChannel::Impl {
#if NEWISP_HAVE_HID
        hid_device* dev = nullptr;
#endif
    };

    HidChannel::HidChannel() : m_impl(new Impl()) {}
    HidChannel::~HidChannel() { Close(); }

    bool HidChannel::Open(const std::string& portName, uint32_t baudRate,
        bool /*evenParity*/)
    {
        Close();
        if (portName.empty()) return false;

        m_currentBaud = baudRate;

#if NEWISP_HAVE_HID
        if (hid_init() != 0) return false;

        hid_device* dev = hid_open_path(portName.c_str());
        if (!dev) return false;

        m_impl->dev = dev;
        m_reportSize = STC_HID_REPORT_SIZE;
        m_inputReportSize = STC_HID_REPORT_SIZE;
        m_portName = portName;

        Flush();
        return true;
#else
        (void)portName;
        return false;
#endif
    }

    void HidChannel::Close()
    {
#if NEWISP_HAVE_HID
        if (m_impl && m_impl->dev) {
            hid_close(m_impl->dev);
            m_impl->dev = nullptr;
        }
#endif
        m_currentBaud = 0;
    }

    bool HidChannel::IsOpen() const
    {
#if NEWISP_HAVE_HID
        return m_impl && m_impl->dev != nullptr;
#else
        return false;
#endif
    }

    bool HidChannel::SetBaudRate(uint32_t baudRate)
    {
        if (!IsOpen()) return false;
        m_currentBaud = baudRate;
        return true;
    }

    bool HidChannel::SetParity(bool /*evenParity*/) { return IsOpen(); }
    bool HidChannel::SetDtr(bool /*asserted*/) { return IsOpen(); }
    bool HidChannel::SetRts(bool /*asserted*/) { return IsOpen(); }

    bool HidChannel::PulseReset(int /*pin*/, uint32_t /*holdMs*/,
        uint32_t /*releaseMs*/)
    {
        return IsOpen();
    }

    bool HidChannel::Write(const std::vector<uint8_t>& data)
    {
        if (!IsOpen()) return false;

#if NEWISP_HAVE_HID
        // The first byte of an outgoing transfer is always reserved for the
        // report ID, on every backend: hidapi documents it that way and both
        // hid.dll and hidraw expect it. (The *read* side is asymmetric -- see
        // the note in Read() -- but writes are unanimous.)
        const size_t reportSize = m_reportSize ? m_reportSize : STC_HID_REPORT_SIZE;
        if (reportSize < 2) return false;
        const size_t payloadSize = reportSize - 1;

        size_t offset = 0;
        do {
            std::vector<uint8_t> report(reportSize, 0x00);   // report ID 0
            size_t chunk = data.size() - offset;
            if (chunk > payloadSize) chunk = payloadSize;
            if (chunk > 0) {
                std::copy(data.begin() + (ptrdiff_t)offset,
                    data.begin() + (ptrdiff_t)(offset + chunk),
                    report.begin() + 1);
            }

            int rc = hid_write(m_impl->dev, report.data(), report.size());
            if (rc < 0) return false;

            offset += chunk;
        } while (offset < data.size());

        return true;
#else
        (void)data;
        return false;
#endif
    }

    std::vector<uint8_t> HidChannel::Read(size_t maxBytes, uint32_t timeoutMs)
    {
        std::vector<uint8_t> result;
        if (!IsOpen()) return result;

#if NEWISP_HAVE_HID
        // Callers written against the serial transport pass 0 to mean "poll
        // once and return immediately". A HID read cannot complete without
        // waiting for the device, so give a zero timeout a short, bounded wait
        // instead of blocking forever.
        int effectiveTimeout = (timeoutMs == 0) ? 20 : (int)timeoutMs;

        const size_t reportSize =
            m_inputReportSize ? m_inputReportSize : STC_HID_REPORT_SIZE;
        std::vector<uint8_t> buffer(reportSize, 0x00);

        int n = hid_read_timeout(m_impl->dev, buffer.data(), buffer.size(),
            effectiveTimeout);
        if (n <= 0) return result;

        // Whether the first byte of the buffer is a report ID depends on the
        // backend, and getting it wrong shifts the whole packet by one byte so
        // that nothing parses.
        //
        //   Windows (hid.dll through hidapi)  the HID stack reserves the first
        //                                     byte for the report ID, so the
        //                                     payload starts at index 1.
        //   Linux hidraw                      the kernel hands over the report
        //                                     body only; there is no report ID
        //                                     byte, and the payload starts at
        //                                     index 0.
        //
        // Rather than compile in an assumption, look at the data: every STC ISP
        // packet begins with the 0x46 0xB9 frame marker, and a report ID of 0
        // would put that marker at index 1 instead.
        size_t payloadStart = 0;
        if (n >= 2 && buffer[0] == 0x46 && buffer[1] == 0xB9) {
            payloadStart = 0;
        }
        else if (n >= 3 && buffer[1] == 0x46 && buffer[2] == 0xB9) {
            payloadStart = 1;
        }
        else {
            // Not a framed reply at all; hand back what arrived so the caller's
            // log can show it, minus a leading report ID if there is one.
            payloadStart = 0;
        }

        size_t available = (size_t)n - payloadStart;
        size_t payloadLen = available;
        if (payloadLen > maxBytes) payloadLen = maxBytes;
        result.assign(buffer.begin() + (ptrdiff_t)payloadStart,
            buffer.begin() + (ptrdiff_t)(payloadStart + payloadLen));
        return result;
#else
        (void)maxBytes; (void)timeoutMs;
        return result;
#endif
    }

    void HidChannel::Flush()
    {
        if (!IsOpen()) return;

#if NEWISP_HAVE_HID
        std::vector<uint8_t> buffer(
            m_inputReportSize ? m_inputReportSize : STC_HID_REPORT_SIZE, 0x00);
        for (int i = 0; i < 64; ++i) {
            int n = hid_read_timeout(m_impl->dev, buffer.data(), buffer.size(), 0);
            if (n <= 0) break;
        }
#endif
    }

#endif  // _WIN32

    bool HidChannel::Reopen(uint32_t baudRate, bool evenParity)
    {
        if (m_portName.empty()) return false;
        std::string name = m_portName;
        return Open(name, baudRate, evenParity);
    }

} // namespace stcisp
