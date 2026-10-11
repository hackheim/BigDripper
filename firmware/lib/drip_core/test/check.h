#pragma once
// A minimal test harness shared by the host tests: TEST(name) { CHECK(...); }
// registers a test, test_main.cpp runs them all.
#include <cstdio>
#include <vector>

extern int failures;
extern const char *current_test;

#define CHECK(cond)                                                              \
  do {                                                                           \
    if (!(cond)) {                                                               \
      printf("FAIL  %s: %s (line %d)\n", current_test, #cond, __LINE__);         \
      failures++;                                                                \
    }                                                                            \
  } while (0)

#define CHECK_EQ(a, b)                                                           \
  do {                                                                           \
    long long va = (long long)(a), vb = (long long)(b);                          \
    if (va != vb) {                                                              \
      printf("FAIL  %s: %s == %s (%lld vs %lld, line %d)\n", current_test, #a,   \
             #b, va, vb, __LINE__);                                              \
      failures++;                                                                \
    }                                                                            \
  } while (0)

typedef void (*TestFn)();
struct Test {
  const char *name;
  TestFn fn;
};
std::vector<Test> &tests();
struct Register {
  Register(const char *name, TestFn fn) { tests().push_back({name, fn}); }
};
#define TEST(name)                              \
  static void name();                           \
  static Register register_##name(#name, name); \
  static void name()
