#pragma once
// Internal definition of SerialChannel's platform state.
//
// This header exists so that the platform-independent parts of SerialChannel
// (the constructor and the reset pulse, which is written purely in terms of
// SetDtr/SetRts/SleepMs) can live in serial_channel.cpp while the platform
// bodies live in serial_channel_win32.cpp / serial_channel_posix.cpp.
//
// It is a private header: only the three transport translation units include
// it, so the Win32 HANDLE / POSIX fd never leaks into the public interface.

#include "newisp/serial_channel.h"

#if defined(_WIN32)
#include <windows.h>
#else
// No POSIX headers are needed here: an int descriptor needs no definition.
#endif

namespace stcisp {

    struct SerialChannel::Impl {
#if defined(_WIN32)
        // INVALID_HANDLE_VALUE rather than nullptr, because that is what
        // CreateFile returns and what CloseHandle must not be given.
        HANDLE handle = INVALID_HANDLE_VALUE;
#else
        int fd = -1;
#endif
    };

} // namespace stcisp
