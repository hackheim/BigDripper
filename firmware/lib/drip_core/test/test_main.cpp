#include "check.h"

int failures = 0;
const char *current_test = "";

std::vector<Test> &tests() {
  static std::vector<Test> t;
  return t;
}

int main() {
  for (const Test &t : tests()) {
    current_test = t.name;
    int before = failures;
    t.fn();
    printf("%s  %s\n", failures == before ? "ok  " : "FAIL", t.name);
  }
  printf("\n%d failure(s)\n", failures);
  return failures ? 1 : 0;
}
