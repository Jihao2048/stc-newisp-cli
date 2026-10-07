# Unit tests.
#
# These cover the pieces that are pure logic: the Intel HEX parser, the chip
# table lookup, the family predicates and the packet framing/checksum code. The
# protocol exchanges that need a live link are covered end-to-end by
# tests/pty_test.py instead, because a fake socket cannot stand in for a tty's
# timing.
#
# No test framework is used: the cases are a handful of assertions, and pulling
# in GoogleTest or Catch2 would make the build depend on fetching something for
# no real gain. Each test file is a small program that returns non-zero on
# failure, which is all CTest needs.

add_executable(newisp_tests
    test_hex_file.cpp
    test_chip_table.cpp
    test_chip_logic.cpp
    test_protocol_framing.cpp
    test_main.cpp
)

target_link_libraries(newisp_tests PRIVATE newisp_transport)

add_test(NAME newisp_tests COMMAND newisp_tests)
