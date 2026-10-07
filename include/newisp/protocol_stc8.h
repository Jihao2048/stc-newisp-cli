#pragma once
// STC32 / STC8G / STC8 protocol implementations, split by transport.
//
// The serial and USB HID builds diverge in more than a few branches -- the
// status-packet framing, the calibration exchange and the source of the IRC
// trim values are all different -- so they live in separate files rather than
// sharing one class full of `if (isHid)`:
//
//   protocol_stc8_uart.h   ProtocolStc32Uart / ProtocolStc8GUart / ProtocolStc8Uart
//   protocol_stc8_hid.h    ProtocolStc32Hid  / ProtocolStc8GHid  / ProtocolStc8Hid
//
// The application lets the user switch transports at run time, so both sets
// are compiled in and the factory picks the matching one. Callers that need a
// concrete class name will find the Uart/Hid suffix; everything that goes
// through IStcProtocol stays unaware of the split.
//
// The other protocol families (STC89/12/15) are serial-only and are not split.

#include "newisp/protocol_stc8_uart.h"
#include "newisp/protocol_stc8_hid.h"