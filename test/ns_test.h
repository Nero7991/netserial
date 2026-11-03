// Minimal assert runner for the portable core. Each suite is a function registered in test_main.c.
#pragma once
#include <stdio.h>

extern int ns_test_failures;

#define CHECK(cond) do { if (!(cond)) { ns_test_failures++; \
    printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECK_EQ(a, b) do { long long _a = (long long)(a), _b = (long long)(b); if (_a != _b) { \
    ns_test_failures++; printf("  FAIL %s:%d: %s == %s (%lld != %lld)\n", __FILE__, __LINE__, \
    #a, #b, _a, _b); } } while (0)
