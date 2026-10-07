// Serial transport for Linux and macOS.
//
// The Win32 backend drives the port through a DCB and EscapeCommFunction. The
// POSIX equivalent is termios for line coding plus the TIOCMGET / TIOCMSET
// ioctls for DTR and RTS. The behaviour is deliberately kept identical:
//
//   * 8N1 or 8E1, no flow control, raw mode (no line discipline processing --
//     without cfmakeraw the tty would translate and buffer bytes and the
//     0x7F wakeup spam would never arrive intact)
//   * reads are non-blocking; a zero timeout polls, a non-zero one waits
//   * DTR and RTS follow whatever the application last asked for, so the reset
//     pulse toggles real pins
//
// One platform difference is worth naming: opening a tty asserts DTR by
// default, and on many USB-serial bridges (CH340, CP2102, FT232) asserting DTR
// is exactly what drives the board's reset line. The Win32 backend clears DTR
// immediately after opening for the same reason; this one does too.

#include "newisp/serial_channel.h"
#include "newisp/text.h"

#include "serial_channel_impl.h"

#include <fcntl.h>
#include <termios.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <cerrno>
#include <cstring>

namespace stcisp {

    namespace {

        // Map a numeric baud rate to the termios constant.
        //
        // Not every rate has a B#### constant on every system: B230400 and
        // above are missing on some older macOS headers, and Linux gained
        // B500000+ well after the classic set. Where the constant is absent the
        // caller gets false rather than a silently wrong speed, because a
        // mismatched baud rate makes the ISP handshake fail in a way that looks
        // like a dead board.
        bool BaudToSpeed(uint32_t baud, speed_t& out)
        {
            switch (baud) {
            case 2400:   out = B2400;   return true;
            case 4800:   out = B4800;   return true;
            case 9600:   out = B9600;   return true;
            case 19200:  out = B19200;  return true;
            case 38400:  out = B38400;  return true;
            case 57600:  out = B57600;  return true;
            case 115200: out = B115200; return true;
#ifdef B230400
            case 230400: out = B230400; return true;
#endif
#ifdef B460800
            case 460800: out = B460800; return true;
#endif
#ifdef B921600
            case 921600: out = B921600; return true;
#endif
            default: return false;
            }
        }

        // Why the line coding was applied, so the caller can log something
        // useful instead of a bare failure.
        enum class CodingResult {
            Ok,
            BaudUnsupported,   // no B#### constant for this rate
            Rejected,          // the driver refused the settings (errno set)
        };

        const char* CodingResultText(CodingResult r)
        {
            switch (r) {
            case CodingResult::Ok:             return "ok";
            case CodingResult::BaudUnsupported: return "baud rate not supported by termios";
            case CodingResult::Rejected:       return "driver rejected the line settings";
            }
            return "unknown";
        }

        // Apply 8N1 / 8E1 raw settings to an already-open descriptor.
        //
        // The parity bit is applied on a best-effort basis. Some tty devices --
        // pseudo-terminals in particular, but also a few USB bridge drivers --
        // reject PARENB with EINVAL. Failing the whole call for that would make
        // the port unusable when the chip does not actually need parity, which
        // is the common case for the older STC families, so the data format and
        // speed are applied first and the parity is attempted afterwards.
        CodingResult ApplyLineCoding(int fd, uint32_t baud, bool evenParity,
            bool* parityApplied)
        {
            speed_t speed = B115200;
            if (!BaudToSpeed(baud, speed)) return CodingResult::BaudUnsupported;

            termios tio;
            if (tcgetattr(fd, &tio) != 0) return CodingResult::Rejected;

            cfmakeraw(&tio);            // no ICANON/ECHO/ISIG, no ICRNL
            tio.c_cflag &= ~(CSIZE | PARENB | PARODD | CSTOPB | CRTSCTS);
            tio.c_cflag |= CS8;         // 8 data bits
            tio.c_cflag |= CLOCAL;      // ignore modem control lines
            tio.c_cflag |= CREAD;       // enable receiver
            // Parity errors must not become NUL bytes in the stream; the STC
            // ISP framing has its own checksum and rejecting bytes here would
            // corrupt packets rather than catch errors.
            tio.c_iflag &= ~(INPCK | ISTRIP);

            tio.c_cc[VMIN] = 0;         // non-blocking read semantics
            tio.c_cc[VTIME] = 0;

            if (cfsetispeed(&tio, speed) != 0) return CodingResult::Rejected;
            if (cfsetospeed(&tio, speed) != 0) return CodingResult::Rejected;

            // Attempt 1: everything, including parity.
            if (evenParity) tio.c_cflag |= PARENB;
            if (tcsetattr(fd, TCSANOW, &tio) == 0) {
                if (parityApplied) *parityApplied = true;
                return CodingResult::Ok;
            }

            if (!evenParity) {
                // Nothing was optional in this request, so the failure is real.
                return CodingResult::Rejected;
            }

            // Attempt 2: drop the parity bit and retry, so at least the speed
            // and framing take effect. The caller is told the parity did not
            // stick via parityApplied.
            tio.c_cflag &= ~PARENB;
            if (tcsetattr(fd, TCSANOW, &tio) != 0) return CodingResult::Rejected;

            if (parityApplied) *parityApplied = false;
            return CodingResult::Ok;
        }

        // Read / write the modem control bits. TIOCMGET is supported by
        // Linux's tty layer and by macOS's serial driver; if it fails the
        // control-line calls become no-ops that report failure, which is the
        // honest answer.
        bool GetModemBits(int fd, int& bits)
        {
            return ioctl(fd, TIOCMGET, &bits) == 0;
        }

        bool SetModemBits(int fd, int set, int clear)
        {
            int bits = 0;
            if (!GetModemBits(fd, bits)) return false;
            bits |= set;
            bits &= ~clear;
            return ioctl(fd, TIOCMSET, &bits) == 0;
        }

        // tcflush on both directions, matching the Win32 PURGE_RXCLEAR |
        // PURGE_TXCLEAR.
        void PurgeBoth(int fd)
        {
            tcflush(fd, TCIOFLUSH);
        }

    } // namespace

    bool SerialChannel::Open(const std::string& portName, uint32_t baudRate,
        bool evenParity)
    {
        Close();

        // O_NONBLOCK on open: without it, opening a tty whose DCD is low blocks
        // until carrier appears, which never happens for a self-powered board.
        // The flag is cleared afterwards so writes block normally.
        int fd = ::open(portName.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
        if (fd < 0) return false;

        bool parityApplied = false;
        CodingResult cr = ApplyLineCoding(fd, baudRate, evenParity,
            &parityApplied);
        if (cr != CodingResult::Ok) {
            // Nothing useful can be done with a port whose speed the driver
            // will not accept; the handshake would run at the wrong rate and
            // fail in a way that looks like a dead chip.
            ::close(fd);
            return false;
        }

        // Drop the O_NONBLOCK used for open; blocking writes are what we want,
        // and reads are bounded by select() below.
        int flags = fcntl(fd, F_GETFL, 0);
        if (flags >= 0) fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);

        // Mirror the Win32 backend, which clears DTR and RTS right after
        // opening. Leaving them asserted would hold the board in reset.
        SetModemBits(fd, 0, TIOCM_DTR);
        SetModemBits(fd, 0, TIOCM_RTS);

        PurgeBoth(fd);

        m_impl->fd = fd;
        m_currentBaud = baudRate;
        // Record what the port is actually doing, not what was asked for: the
        // protocol layer reads EvenParity() to decide whether to re-apply it,
        // and reporting a parity the driver refused would make that decision
        // wrong.
        m_evenParity = parityApplied;
        m_portName = portName;
        return true;
    }

    void SerialChannel::Close()
    {
        if (m_impl && m_impl->fd >= 0) {
            ::close(m_impl->fd);
            m_impl->fd = -1;
        }
        m_currentBaud = 0;
    }

    bool SerialChannel::IsOpen() const
    {
        return m_impl && m_impl->fd >= 0;
    }

    bool SerialChannel::SetBaudRate(uint32_t baudRate)
    {
        if (!IsOpen()) return false;
        if (baudRate == m_currentBaud) return true;

        bool parityApplied = false;
        CodingResult cr = ApplyLineCoding(m_impl->fd, baudRate, m_evenParity,
            &parityApplied);
        if (cr != CodingResult::Ok) return false;

        PurgeBoth(m_impl->fd);
        m_currentBaud = baudRate;
        m_evenParity = parityApplied;
        return true;
    }

    bool SerialChannel::SetParity(bool evenParity)
    {
        if (!IsOpen()) return false;
        if (evenParity == m_evenParity) return true;

        bool parityApplied = false;
        CodingResult cr = ApplyLineCoding(m_impl->fd, m_currentBaud, evenParity,
            &parityApplied);
        if (cr != CodingResult::Ok) return false;

        PurgeBoth(m_impl->fd);
        m_evenParity = parityApplied;

        // Report honestly: a driver that refused the parity bit must not be
        // described as having accepted it, or the caller will assume the line
        // is configured for a family that needs even parity.
        return parityApplied == evenParity;
    }

    bool SerialChannel::SetDtr(bool asserted)
    {
        if (!IsOpen()) return false;
        return asserted ? SetModemBits(m_impl->fd, TIOCM_DTR, 0)
                        : SetModemBits(m_impl->fd, 0, TIOCM_DTR);
    }

    bool SerialChannel::SetRts(bool asserted)
    {
        if (!IsOpen()) return false;
        return asserted ? SetModemBits(m_impl->fd, TIOCM_RTS, 0)
                        : SetModemBits(m_impl->fd, 0, TIOCM_RTS);
    }

    bool SerialChannel::Write(const std::vector<uint8_t>& data)
    {
        if (!IsOpen() || data.empty()) return false;

        // Writes are blocking (O_NONBLOCK was cleared), but a short write is
        // still possible and must not be reported as success: a truncated ISP
        // packet looks like a chip that stopped answering.
        size_t written = 0;
        while (written < data.size()) {
            ssize_t n = ::write(m_impl->fd, data.data() + written,
                data.size() - written);
            if (n < 0) {
                if (errno == EINTR) continue;
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    // Output buffer full; give the driver a moment to drain.
                    newisp::SleepMs(1);
                    continue;
                }
                return false;
            }
            written += (size_t)n;
        }

        // tcdrain is the POSIX spelling of FlushFileBuffers: wait until the
        // bytes have actually left the port. The ISP handshake depends on the
        // host not racing ahead of the wire.
        tcdrain(m_impl->fd);
        return true;
    }

    std::vector<uint8_t> SerialChannel::Read(size_t maxBytes, uint32_t timeoutMs)
    {
        std::vector<uint8_t> result;
        if (!IsOpen() || maxBytes == 0) return result;

        // VMIN = 0 makes read() return immediately when nothing is buffered,
        // so the timeout is implemented with select() rather than VTIME. That
        // keeps a zero-timeout poll genuinely instantaneous, which is what the
        // protocol layer's PulseUntilData depends on.
        if (timeoutMs > 0) {
            fd_set rfds;
            FD_ZERO(&rfds);
            FD_SET(m_impl->fd, &rfds);

            timeval tv;
            tv.tv_sec = timeoutMs / 1000;
            tv.tv_usec = (timeoutMs % 1000) * 1000;

            int rc = ::select(m_impl->fd + 1, &rfds, nullptr, nullptr, &tv);
            if (rc <= 0) return result;   // timeout or error: no data
        }

        std::vector<uint8_t> buffer(maxBytes);
        ssize_t n = ::read(m_impl->fd, buffer.data(), maxBytes);
        if (n > 0) {
            buffer.resize((size_t)n);
            return buffer;
        }
        return result;
    }

    void SerialChannel::Flush()
    {
        if (IsOpen()) PurgeBoth(m_impl->fd);
    }

} // namespace stcisp
