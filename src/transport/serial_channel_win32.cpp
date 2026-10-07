// Serial transport for Windows.
//
// This is the original UartChannel implementation, kept verbatim apart from
// string types: the Win32 DCB settings are the ones the ISP monitor actually
// needs, and changing them is a good way to break the handshake.

#include "newisp/serial_channel.h"
#include "newisp/text.h"

#include "serial_channel_impl.h"

#include <windows.h>

#include <algorithm>

namespace stcisp {

    namespace {

        // COM port names and device paths are ASCII in practice; convert to
        // UTF-16 at the API boundary.
        std::wstring ToWide(const std::string& s)
        {
            if (s.empty()) return std::wstring();
            int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(),
                nullptr, 0);
            std::wstring w((size_t)n, L'\0');
            MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
            return w;
        }

    } // namespace

    bool SerialChannel::Open(const std::string& portName, uint32_t baudRate,
        bool evenParity)
    {
        Close();

        // "\\\\.\\COM10" is required for ports above COM9, and harmless below.
        std::wstring fullPath = L"\\\\.\\" + ToWide(portName);
        HANDLE h = CreateFileW(fullPath.c_str(),
            GENERIC_READ | GENERIC_WRITE,
            0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return false;

        EscapeCommFunction(h, CLRDTR);
        EscapeCommFunction(h, CLRRTS);

        DCB dcb = {};
        dcb.DCBlength = sizeof(DCB);
        if (!GetCommState(h, &dcb)) { CloseHandle(h); return false; }

        dcb.BaudRate = baudRate;
        dcb.ByteSize = 8;
        dcb.Parity = evenParity ? EVENPARITY : NOPARITY;
        dcb.fParity = evenParity ? TRUE : FALSE;
        dcb.StopBits = ONESTOPBIT;
        dcb.fBinary = TRUE;
        dcb.fOutxCtsFlow = FALSE;
        dcb.fOutxDsrFlow = FALSE;

        // DTR_CONTROL_ENABLE is what makes EscapeCommFunction(SETDTR/CLRDTR)
        // take effect; the reset pulse silently does nothing otherwise.
        dcb.fDtrControl = DTR_CONTROL_ENABLE;
        dcb.fRtsControl = RTS_CONTROL_ENABLE;

        dcb.fDsrSensitivity = FALSE;
        dcb.fOutX = FALSE;
        dcb.fInX = FALSE;
        dcb.fAbortOnError = FALSE;

        if (!SetCommState(h, &dcb)) {
            // A driver that will not accept parity at all should not make the
            // port unusable: retry without it, and remember what actually
            // stuck. The older STC families run at 8N1 anyway, and reporting a
            // parity the port does not have would make the caller's
            // parity check give the wrong answer.
            if (!evenParity) { CloseHandle(h); return false; }

            dcb.Parity = NOPARITY;
            dcb.fParity = FALSE;
            if (!SetCommState(h, &dcb)) { CloseHandle(h); return false; }
            evenParity = false;
        }

        // Polled reads: return whatever has arrived, immediately.
        COMMTIMEOUTS timeouts = {};
        timeouts.ReadIntervalTimeout = MAXDWORD;
        timeouts.ReadTotalTimeoutMultiplier = 0;
        timeouts.ReadTotalTimeoutConstant = 0;
        timeouts.WriteTotalTimeoutMultiplier = 0;
        timeouts.WriteTotalTimeoutConstant = 2000;
        SetCommTimeouts(h, &timeouts);

        PurgeComm(h, PURGE_RXCLEAR | PURGE_TXCLEAR);

        m_impl->handle = h;
        m_currentBaud = baudRate;
        // Record what the port is actually doing, not what was requested.
        m_evenParity = evenParity;
        m_portName = portName;
        return true;
    }

    void SerialChannel::Close()
    {
        if (m_impl && m_impl->handle != INVALID_HANDLE_VALUE) {
            CloseHandle(m_impl->handle);
            m_impl->handle = INVALID_HANDLE_VALUE;
        }
        m_currentBaud = 0;
    }

    bool SerialChannel::IsOpen() const
    {
        return m_impl && m_impl->handle != INVALID_HANDLE_VALUE;
    }

    bool SerialChannel::SetBaudRate(uint32_t baudRate)
    {
        if (!IsOpen()) return false;
        if (baudRate == m_currentBaud) return true;

        HANDLE h = m_impl->handle;
        DCB dcb = {};
        dcb.DCBlength = sizeof(DCB);
        if (!GetCommState(h, &dcb)) return false;

        dcb.BaudRate = baudRate;
        dcb.ByteSize = 8;
        dcb.Parity = m_evenParity ? EVENPARITY : NOPARITY;
        dcb.fParity = m_evenParity ? TRUE : FALSE;
        dcb.StopBits = ONESTOPBIT;
        dcb.fBinary = TRUE;
        dcb.fDtrControl = DTR_CONTROL_ENABLE;
        dcb.fRtsControl = RTS_CONTROL_ENABLE;

        if (!SetCommState(h, &dcb)) return false;
        PurgeComm(h, PURGE_RXCLEAR | PURGE_TXCLEAR);
        m_currentBaud = baudRate;
        return true;
    }

    bool SerialChannel::SetParity(bool evenParity)
    {
        if (!IsOpen()) return false;
        if (evenParity == m_evenParity) return true;

        HANDLE h = m_impl->handle;
        DCB dcb = {};
        dcb.DCBlength = sizeof(DCB);
        if (!GetCommState(h, &dcb)) return false;

        dcb.Parity = evenParity ? EVENPARITY : NOPARITY;
        dcb.fParity = evenParity ? TRUE : FALSE;
        dcb.ByteSize = 8;
        dcb.StopBits = ONESTOPBIT;
        dcb.fBinary = TRUE;
        dcb.fDtrControl = DTR_CONTROL_ENABLE;
        dcb.fRtsControl = RTS_CONTROL_ENABLE;

        if (!SetCommState(h, &dcb)) {
            // Some USB-serial bridge drivers refuse a parity change once the
            // port is open. Retry without it so the speed and framing at least
            // stay correct, and report the truth rather than claiming success:
            // the caller tests EvenParity() before deciding whether the line is
            // already configured for the detected family.
            dcb.Parity = NOPARITY;
            dcb.fParity = FALSE;
            if (!SetCommState(h, &dcb)) return false;

            PurgeComm(h, PURGE_RXCLEAR | PURGE_TXCLEAR);
            m_evenParity = false;
            return false;
        }

        PurgeComm(h, PURGE_RXCLEAR | PURGE_TXCLEAR);
        m_evenParity = evenParity;
        return true;
    }

    bool SerialChannel::SetDtr(bool asserted)
    {
        if (!IsOpen()) return false;
        return EscapeCommFunction(m_impl->handle,
            asserted ? SETDTR : CLRDTR) != FALSE;
    }

    bool SerialChannel::SetRts(bool asserted)
    {
        if (!IsOpen()) return false;
        return EscapeCommFunction(m_impl->handle,
            asserted ? SETRTS : CLRRTS) != FALSE;
    }

    bool SerialChannel::Write(const std::vector<uint8_t>& data)
    {
        if (!IsOpen() || data.empty()) return false;
        DWORD written = 0;
        BOOL ok = WriteFile(m_impl->handle,
            data.data(), (DWORD)data.size(), &written, nullptr);
        if (ok && written == data.size()) {
            FlushFileBuffers(m_impl->handle);
            return true;
        }
        return false;
    }

    std::vector<uint8_t> SerialChannel::Read(size_t maxBytes, uint32_t /*timeoutMs*/)
    {
        std::vector<uint8_t> buffer(maxBytes);
        if (!IsOpen()) return {};
        DWORD read = 0;
        ReadFile(m_impl->handle, buffer.data(), (DWORD)maxBytes, &read, nullptr);
        buffer.resize(read);
        return buffer;
    }

    void SerialChannel::Flush()
    {
        if (IsOpen()) {
            PurgeComm(m_impl->handle, PURGE_RXCLEAR | PURGE_TXCLEAR);
        }
    }

} // namespace stcisp
