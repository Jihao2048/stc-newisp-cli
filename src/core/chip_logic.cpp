// Non-inline parts of the chip logic.
//
// chip_logic.h is almost entirely inline tables and predicates, which is how it
// was written in the WinUI build and is fine: the tables are constexpr-ish
// data, and inlining the predicates keeps the call sites readable.
//
// What cannot live in a header is anything that would give the program several
// definitions. At present that is nothing, so this file exists to hold the
// translation unit that CMake compiles and to give a home to future additions.
//
// The one deliberate exception is the chip table itself: chip_table.h declares
// 1300 entries as a static table, and ChipNameFromTable is defined out of line
// in chip_table.cpp so the table is not copied into every includer.

#include "newisp/chip_logic.h"

namespace newisp {

    // Intentionally empty for now. Keeping the translation unit means the
    // CMake target has a stable source list and adding a real function here
    // later does not require touching the build.

} // namespace newisp
