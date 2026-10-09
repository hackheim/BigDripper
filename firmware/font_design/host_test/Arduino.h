// Just enough of Arduino.h to compile font.cpp + font_data.cpp on the host.
#pragma once
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <string>

struct String : std::string {
  using std::string::string;
  String(const std::string &s) : std::string(s) {}
  size_t length() const { return size(); }
};

#define log_i(fmt, ...) printf(fmt "\n", ##__VA_ARGS__)
