/*
 * Copyright (c) 2026 COBALT Technologies
 * SPDX-License-Identifier: Apache-2.0
 *
 * A few macros for the host tests of the pure logic of firmware/lib: no test
 * framework to install, gcc and make are enough (WSL on the bench PC, Linux in
 * the CI of lot A5).
 */

#ifndef CB91AI_HARNESS_H
#define CB91AI_HARNESS_H

#include <stdio.h>

static int harness_checks;
static int harness_failures;

#define CHECK(cond)                                                                                \
	do {                                                                                       \
		harness_checks++;                                                                  \
		if (!(cond)) {                                                                     \
			harness_failures++;                                                        \
			printf("  %s:%d: failed: %s\n", __FILE__, __LINE__, #cond);                \
		}                                                                                  \
	} while (0)

#define CHECK_EQ(a, b)                                                                             \
	do {                                                                                       \
		const long long harness_a = (long long)(a);                                        \
		const long long harness_b = (long long)(b);                                        \
		harness_checks++;                                                                  \
		if (harness_a != harness_b) {                                                      \
			harness_failures++;                                                        \
			printf("  %s:%d: failed: %s == %s (%lld != %lld)\n", __FILE__, __LINE__, #a, \
			       #b, harness_a, harness_b);                                          \
		}                                                                                  \
	} while (0)

#define RUN(test)                                                                                  \
	do {                                                                                       \
		const int harness_before = harness_failures;                                       \
		test();                                                                            \
		printf("%s %s\n", harness_failures == harness_before ? "ok  " : "FAIL", #test);    \
	} while (0)

static inline int harness_report(const char *suite)
{
	printf("%s: %d check(s), %d failure(s)\n", suite, harness_checks, harness_failures);
	return harness_failures == 0 ? 0 : 1;
}

#endif /* CB91AI_HARNESS_H */
