// Host test runner: ./ns_tests [suite...]; no args runs every suite, --list names them.
#include "ns_test.h"
#include <string.h>

int ns_test_failures;

void suite_ring(void);
void suite_frame(void);
void suite_tx(void);
void suite_ring_fixes(void);
void suite_tx_fixes(void);
void suite_lastwords(void);
void suite_hil_fixes(void);

static const struct { const char *name; void (*fn)(void); } SUITES[] = {
    { "ring", suite_ring },
    { "frame", suite_frame },
    { "tx", suite_tx },
    { "ring_fixes", suite_ring_fixes },
    { "tx_fixes", suite_tx_fixes },
    { "lastwords", suite_lastwords },
    { "hil_fixes", suite_hil_fixes },
};
#define N_SUITES (sizeof(SUITES) / sizeof(SUITES[0]))

int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "--list") == 0) {
        for (size_t i = 0; i < N_SUITES; i++) puts(SUITES[i].name);
        return 0;
    }
    for (size_t i = 0; i < N_SUITES; i++) {
        int run = argc == 1;
        for (int a = 1; a < argc; a++) run |= strcmp(argv[a], SUITES[i].name) == 0;
        if (!run) continue;
        printf("%s\n", SUITES[i].name);
        SUITES[i].fn();
    }
    printf("%s (%d failures)\n", ns_test_failures ? "FAILED" : "OK", ns_test_failures);
    return ns_test_failures ? 1 : 0;
}
