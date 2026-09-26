#ifndef PICO_TEST_SANITIZER_DETECT_H
#define PICO_TEST_SANITIZER_DETECT_H

/* Sanitizer runtimes reserve vast virtual address space (ASan shadow memory
 * in particular), so tests that constrain the process with RLIMIT_AS or tight
 * wall-clock budgets must relax those constraints in sanitizer builds. The
 * constraints stay enforced in ordinary debug/release builds. */
#if defined(__SANITIZE_ADDRESS__)
#define PICO_TEST_ASAN 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define PICO_TEST_ASAN 1
#else
#define PICO_TEST_ASAN 0
#endif
#else
#define PICO_TEST_ASAN 0
#endif

#endif
