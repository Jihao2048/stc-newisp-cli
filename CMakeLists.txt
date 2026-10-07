# 3.20 is the floor: it is what Ubuntu 22.04 ships and what the CMake versions
# bundled with recent Visual Studio and Homebrew provide. CMake 4.x refuses
# projects that ask for a policy version below 3.5, so this also keeps the build
# working on the newest toolchains.
cmake_minimum_required(VERSION 3.20)

project(newisp
    VERSION 3.0.0
    DESCRIPTION "STC MCU programmer (serial + USB HID), cross-platform CLI"
    LANGUAGES CXX)

# ---------------------------------------------------------------------------
#  Language level
# ---------------------------------------------------------------------------
# C++17 is the floor: the code uses std::string_view-free constructs but relies
# on inline variables, std::make_unique and if-init. Nothing here needs newer.

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)

# ---------------------------------------------------------------------------
#  Source encoding
# ---------------------------------------------------------------------------
# The log messages in this project are Chinese, and the source files are UTF-8.
# MSVC defaults to the system ANSI code page (936/GBK on a Chinese Windows),
# which mangles those string literals into "newline in constant" errors.
#
# Two things are done about it, because either alone is fragile:
#
#   * /utf-8 tells MSVC the source and the execution character set are both
#     UTF-8. This is the authoritative fix.
#   * Every file is saved with a UTF-8 BOM, which is what Visual Studio itself
#     writes and what GCC/Clang also accept. It keeps the files unambiguous
#     even when compiled by a tool that ignores the flag.

if(MSVC)
    add_compile_options(/utf-8 /W4 /permissive-)
    # /W4 is noisy about the C++ standard library on some MSVC versions; the
    # ported protocol code is C++17-clean, so keep the external headers quiet.
    add_compile_options(/external:W0)
else()
    add_compile_options(-Wall -Wextra)
endif()

# ---------------------------------------------------------------------------
#  Options
# ---------------------------------------------------------------------------
# HID support. On Windows it needs nothing external: the backend talks to the
# system HID class driver through hid.dll and SetupAPI, both of which ship with
# the operating system. On macOS and Linux it uses hidapi, which is packaged
# everywhere and wraps IOKit and hidraw respectively.
#
# On those two platforms hidapi is optional: if it is missing, the build still
# succeeds and produces a serial-only binary that reports HID as unavailable.
# That is deliberate -- a missing optional dependency should not be a link
# error, and serial alone is a working tool.

option(NEWISP_ENABLE_HID "Build the USB HID transport" ON)
option(NEWISP_BUILD_TESTS "Build the unit tests" OFF)

if(NEWISP_ENABLE_HID AND NOT WIN32)
    find_package(hidapi QUIET)
    if(NOT hidapi_FOUND)
        # pkg-config fallback: most distributions ship hidapi this way and the
        # CMake config package is comparatively recent.
        find_package(PkgConfig QUIET)
        if(PkgConfig_FOUND)
            pkg_check_modules(HIDAPI QUIET hidapi-hidraw)
            if(NOT HIDAPI_FOUND)
                pkg_check_modules(HIDAPI QUIET hidapi)
            endif()
        endif()
    endif()

    if(NOT hidapi_FOUND AND NOT HIDAPI_FOUND)
        message(STATUS "hidapi not found -- building without USB HID support")
        set(NEWISP_ENABLE_HID OFF)
    endif()
endif()

# ---------------------------------------------------------------------------
#  Core library: chip tables, Intel HEX parsing, ISP protocols
# ---------------------------------------------------------------------------
# This target is pure C++ with no platform dependencies at all, which is what
# lets the same protocol code build on all three platforms.

add_library(newisp_core STATIC
    src/core/chip_table.cpp
    src/core/chip_logic.cpp
    src/core/hex_file.cpp
)

target_include_directories(newisp_core
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_compile_features(newisp_core PUBLIC cxx_std_17)

# The protocol layer is header-only and pulls in <thread>; nothing else is
# needed on any of the three platforms.
find_package(Threads REQUIRED)
target_link_libraries(newisp_core PUBLIC Threads::Threads)

# ---------------------------------------------------------------------------
#  Transport library: serial port + USB HID device access
# ---------------------------------------------------------------------------
# Everything platform-specific is confined to this target.

add_library(newisp_transport STATIC
    src/transport/serial_channel.cpp
    src/transport/serial_enum.cpp
    # Compiled whether or not hidapi is available: without it the file provides
    # stubs that report HID as unsupported, so the CLI always links and can say
    # so plainly instead of failing with unresolved symbols.
    src/transport/hid_channel.cpp
)

if(WIN32)
    target_sources(newisp_transport PRIVATE src/transport/serial_channel_win32.cpp)
else()
    target_sources(newisp_transport PRIVATE src/transport/serial_channel_posix.cpp)
endif()

target_link_libraries(newisp_transport PUBLIC newisp_core)

# serial_channel_impl.h is a private header shared by the three transport
# translation units, so the directory has to be on the include path.
target_include_directories(newisp_transport PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/transport)

if(NEWISP_ENABLE_HID)
    target_compile_definitions(newisp_transport PUBLIC NEWISP_HAVE_HID=1)
else()
    # hid_channel.cpp is compiled either way and provides working stubs that
    # report HID as unavailable, so the CLI links and can say so plainly.
    target_compile_definitions(newisp_transport PUBLIC NEWISP_HAVE_HID=0)
endif()

if(WIN32)
    # The Windows HID backend links against the system HID class driver.
    # hid.lib carries both the HidD_* class accessors and the HidP_* report
    # parser (HidP_GetCaps); setupapi covers device enumeration. Neither needs
    # to be shipped, so the Windows build has no external dependency.
    if(NEWISP_ENABLE_HID)
        target_link_libraries(newisp_transport PRIVATE setupapi hid)
    else()
        target_link_libraries(newisp_transport PRIVATE setupapi)
    endif()
elseif(APPLE)
    # IOKit provides the serial device list on macOS.
    find_library(IOKIT_FRAMEWORK IOKit REQUIRED)
    find_library(COREFOUNDATION_FRAMEWORK CoreFoundation REQUIRED)
    target_link_libraries(newisp_transport PRIVATE
        ${IOKIT_FRAMEWORK} ${COREFOUNDATION_FRAMEWORK})
endif()

if(NEWISP_ENABLE_HID AND NOT WIN32)
    if(hidapi_FOUND)
        # Target name differs between the CMake package and older releases.
        if(TARGET hidapi::hidapi)
            target_link_libraries(newisp_transport PUBLIC hidapi::hidapi)
        elseif(TARGET hidapi::hidapi-hidraw)
            target_link_libraries(newisp_transport PUBLIC hidapi::hidapi-hidraw)
        elseif(TARGET hidapi::hidapi-libusb)
            target_link_libraries(newisp_transport PUBLIC hidapi::hidapi-libusb)
        else()
            target_link_libraries(newisp_transport PUBLIC hidapi)
        endif()
    else()
        target_include_directories(newisp_transport PRIVATE ${HIDAPI_INCLUDE_DIRS})
        target_link_directories(newisp_transport PRIVATE ${HIDAPI_LIBRARY_DIRS})
        target_link_libraries(newisp_transport PUBLIC ${HIDAPI_LIBRARIES})
    endif()
endif()

# ---------------------------------------------------------------------------
#  CLI executable
# ---------------------------------------------------------------------------

add_executable(newisp
    src/cli/main.cpp
    src/cli/options.cpp
    src/cli/session.cpp
)

target_link_libraries(newisp PRIVATE newisp_transport)

# Version string for --version and the usage banner.
target_compile_definitions(newisp PRIVATE
    NEWISP_VERSION="${PROJECT_VERSION}")

if(WIN32)
    # Console subsystem: this is a command-line tool, so no WinMain.
    target_link_libraries(newisp PRIVATE setupapi hid)
endif()

install(TARGETS newisp RUNTIME DESTINATION bin)

# ---------------------------------------------------------------------------
#  Tests
# ---------------------------------------------------------------------------

if(NEWISP_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
