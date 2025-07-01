#pragma once

#include <array>
#include <functional>
#include <iostream>
#include <numeric>
#include <span>
#include <string>
#include <vector>

namespace AN {
namespace Log {
// constexpr std::string AnsiEscape = "\x1b";
enum AnsiAttributes {
  Plain,
  No,
  Bright,
  Dim,
  Italic,
  Underline,
  Reverse,
  With,
  AnsiEsc,
  AnsiAttributesSize
};
constexpr std::array<std::string, AnsiAttributesSize> AnsiCodes = {
    "0", "2", "1", "2", "3", "4", "7", ";", "\x1b"};

// std::string applyStyle(std::span<const AnsiAttributes> styles); // private anyways

void printFmt(const std::string_view str, std::vector<AnsiAttributes> styles);

} // namespace Log
} // namespace AN
