#pragma once

#include <string>
#include <unistd.h>
#include <vector>

namespace AN {
namespace Log {

enum AnsiAttributes {
  AnsiPlain,
  AnsiNo,
  AnsiBright,
  AnsiDim,
  AnsiItalic,
  AnsiUnderline,
  AnsiReverse,
  AnsiWith,
  AnsiEsc,
  AnsiAttributesSize
};

void printFmt(const std::string_view str, std::vector<AnsiAttributes> styles);


class Logger {
public:
  // requires fd be an opened file managed elsewhere, this just formats to output
  Logger(int fd = STDIN_FILENO);

  void log(std::string message, bool disable = false);
  void logErr(std::string message, bool disable = false);
  void changeFd(int fd);

private:
  int m_fd;
  bool m_isatty;
};

} // namespace Log
} // namespace AN
