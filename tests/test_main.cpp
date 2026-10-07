// Test runner.
//
// Each suite is a function that registers its own assertions with the macros in
// test.h; this file calls them all and reports the total.

#include "test.h"

#include <cstdio>

void RunHexFileTests();
void RunChipTableTests();
void RunChipLogicTests();
void RunProtocolFramingTests();

int main()
{
    RunHexFileTests();
    RunChipTableTests();
    RunChipLogicTests();
    RunProtocolFramingTests();

    int failures = newisp_test::Failures();
    if (failures == 0) {
        std::printf("all tests passed\n");
        return 0;
    }

    std::printf("%d assertion(s) failed\n", failures);
    return 1;
}
