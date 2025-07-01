#include "log.hpp"

namespace AN {
namespace Log {

std::string applyStyle(std::span<const AnsiAttributes> styles) {
  std::string combinedStyle =
      std::accumulate(styles.begin(), styles.end(), std::string(),
                      [&](std::string str, AnsiAttributes style) {
                        return str + AnsiCodes[style];
                      });
  return AnsiCodes[AnsiEsc] + "[" + combinedStyle + "m";
}

void printFmt(const std::string_view str, std::vector<AnsiAttributes> styles) {
  std::cout << applyStyle(styles) << str;
  styles.emplace(styles.begin(), AnsiAttributes::No);
  std::cout << applyStyle(styles);
}

} // namespace Log
} // namespace AN
