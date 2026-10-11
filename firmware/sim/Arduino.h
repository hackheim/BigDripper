// Just enough of Arduino.h to build font.cpp and settings.cpp for the
// simulator (WebAssembly). Like font_design/host_test/Arduino.h, but logs go
// nowhere: there's no console to print to.
#pragma once
#include <cctype>
#include <cstdint>
#include <string>

struct String : std::string {
  using std::string::string;
  String(const std::string &s) : std::string(s) {}
  size_t length() const { return size(); }
};

#define log_i(...) ((void)0)
