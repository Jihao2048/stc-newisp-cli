// Serial transport: the parts that do not depend on the platform.
//
// Everything that touches the operating system lives in serial_channel_win32.cpp
// (Win32) or serial_channel_posix.cpp (Linux and macOS); exactly one of those is
// compiled. This file holds the constructor and the reset pulse, both of which
// are written purely in terms of the virtual interface.

#include "newisp/serial_channel.h"
#include "newisp/text.h"

#include "serial_channel_impl.h"

namespace stcisp {

    SerialChannel::SerialChannel()
        : m_impl(new Impl())
    {
    }

    SerialChannel::~SerialChannel()
    {
        // Close() is defined in the platform file, where the handle type is
        // known.
        Close();
    }

    bool SerialChannel::Reopen(uint32_t baudRate, bool evenParity)
    {
        if (m_portName.empty()) return false;
        std::string name = m_portName;
        return Open(name, baudRate, evenParity);
    }

    bool SerialChannel::PulseReset(int pin, uint32_t holdMs, uint32_t releaseMs)
    {
        if (!IsOpen()) return false;

        auto set = [&](bool on) -> bool {
            return (pin == 1) ? SetRts(on) : SetDtr(on);
        };

        // Assert, hold, release. On most USB-serial bridges DTR is wired to the
        // board's reset line through a capacitor, so this is what puts the chip
        // into its bootloader window; the hold time matters and is taken from
        // the caller.
        set(true);
        newisp::SleepMs(holdMs);
        set(false);
        newisp::SleepMs(releaseMs);
        return true;
    }

} // namespace stcisp
