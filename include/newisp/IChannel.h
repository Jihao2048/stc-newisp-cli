#pragma once
// Common transport abstraction shared by the serial and USB HID backends.
//
// The protocol layer only ever needs four operations -- Write, Read, Flush and
// SetBaudRate -- plus bookkeeping accessors. Both SerialChannel and HidChannel
// implement that exact set, so the protocol classes are written against
// IChannel instead of a concrete transport.
//
// The remaining members (parity, DTR/RTS, reset pulse) are part of the
// interface because the connection-setup code calls them unconditionally. A
// HID ISP interface has no line coding and no reset line, so HidChannel answers
// those calls with no-ops that report success.
//
// Ported from the WinUI build's IChannel.h. The only change is that device
// identifiers are UTF-8 std::string rather than std::wstring: a COM port name
// ("COM7") and a POSIX device path ("/dev/ttyUSB0") are both ASCII, and the
// Windows layer converts to UTF-16 internally where the API requires it.

#include <string>
#include <vector>
#include <cstdint>
#include <functional>

namespace stcisp {

    // Which physical transport a channel instance speaks.
    enum class ChannelKind {
        Uart,
        Hid,
    };

    class IChannel {
    public:
        virtual ~IChannel() = default;

        IChannel(const IChannel&) = delete;
        IChannel& operator=(const IChannel&) = delete;

        // ---- lifecycle ----
        // portName is a COM port name or POSIX device path for the serial
        // backend, or a HID device interface path for the HID backend.
        // baudRate / evenParity are ignored by HID.
        virtual bool Open(const std::string& portName, uint32_t baudRate,
                          bool evenParity = true) = 0;
        virtual void Close() = 0;
        virtual bool IsOpen() const = 0;

        virtual bool Reopen(uint32_t baudRate, bool evenParity) = 0;

        // ---- line coding ----
        virtual bool SetBaudRate(uint32_t baudRate) = 0;
        virtual bool SetParity(bool evenParity) = 0;

        // ---- control lines ----
        virtual bool SetDtr(bool asserted) = 0;
        virtual bool SetRts(bool asserted) = 0;
        // pin: 0 = DTR, 1 = RTS. No-op on HID.
        virtual bool PulseReset(int pin, uint32_t holdMs = 250,
                                uint32_t releaseMs = 30) = 0;

        // ---- I/O ----
        virtual bool Write(const std::vector<uint8_t>& data) = 0;
        virtual std::vector<uint8_t> Read(size_t maxBytes, uint32_t timeoutMs) = 0;
        virtual void Flush() = 0;

        // ---- accessors ----
        virtual ChannelKind Kind() const = 0;
        virtual bool EvenParity() const = 0;
        virtual uint32_t CurrentBaud() const = 0;
        virtual const std::string& PortName() const = 0;

    protected:
        IChannel() = default;
    };

} // namespace stcisp
