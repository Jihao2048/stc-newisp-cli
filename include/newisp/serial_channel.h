#pragma once
// Serial transport for the classic STC ISP monitor.
//
// The WinUI build's UartChannel.cpp was pure Win32 (CreateFile/DCB/EscapeComm-
// Function). This port keeps that code for Windows and adds a POSIX termios
// backend for Linux and macOS behind the same IChannel interface, so the
// protocol layer above is unchanged.
//
// Behaviour that must match on all platforms, because the ISP monitor depends
// on it:
//
//   * 8 data bits, one stop bit, parity as requested
//   * no flow control (a hardware handshake would stall the 0x7F spam)
//   * DTR and RTS are driven by the application, not by the driver, so the
//     reset pulse actually toggles the pins
//   * reads never block longer than asked: a zero timeout means "poll"

#include <string>
#include <vector>
#include <cstdint>
#include <memory>

#include "newisp/IChannel.h"

namespace stcisp {

    // Descriptor of a discovered serial port.
    struct SerialPortInfo {
        std::string device;       // "COM7" or "/dev/ttyUSB0"
        std::string displayName;  // human-readable, may equal device
    };

    // Enumerate the serial ports currently present. On Windows this walks the
    // SetupAPI port class; on macOS it asks IOKit; on Linux it scans /dev for
    // the usual tty device names.
    std::vector<SerialPortInfo> EnumerateSerialPorts();

    class SerialChannel : public IChannel {
    public:
        SerialChannel();
        ~SerialChannel() override;

        SerialChannel(const SerialChannel&) = delete;
        SerialChannel& operator=(const SerialChannel&) = delete;

        bool Open(const std::string& portName, uint32_t baudRate,
                  bool evenParity = true) override;
        void Close() override;
        bool IsOpen() const override;

        bool Reopen(uint32_t baudRate, bool evenParity) override;

        bool SetBaudRate(uint32_t baudRate) override;
        bool SetParity(bool evenParity) override;

        // Drive DTR / RTS directly.
        bool SetDtr(bool asserted) override;
        bool SetRts(bool asserted) override;

        // Reset pulse: assert the pin for holdMs, release it, then wait
        // releaseMs. pin: 0 = DTR, 1 = RTS.
        bool PulseReset(int pin, uint32_t holdMs = 250,
                        uint32_t releaseMs = 30) override;

        bool Write(const std::vector<uint8_t>& data) override;
        std::vector<uint8_t> Read(size_t maxBytes, uint32_t timeoutMs) override;
        void Flush() override;

        ChannelKind Kind() const override { return ChannelKind::Uart; }
        bool EvenParity() const override { return m_evenParity; }
        uint32_t CurrentBaud() const override { return m_currentBaud; }
        const std::string& PortName() const override { return m_portName; }

    private:
        struct Impl;
        std::unique_ptr<Impl> m_impl;
        uint32_t    m_currentBaud = 0;
        bool        m_evenParity = true;
        std::string m_portName;
    };

} // namespace stcisp
