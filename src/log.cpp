#include "Log.hpp"

#include <cstdlib>
#include <fcntl.h>
#include <string>
#include <unistd.h>
#include <vector>

namespace AN {
namespace Log {

constexpr std::array<std::string, AnsiAttributesSize> AnsiCodes = {
    "0", "2", "1", "2", "3", "4", "7", ";", "\x1b"};

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
  styles.emplace(styles.begin(), AnsiAttributes::AnsiNo);
  std::cout << applyStyle(styles) << "\n";
}

Logger::Logger(int fd)
    :m_fd(fd) {
  changeFd(fd);
}

void Logger::changeFd(int fd) {
  if (fd != m_fd)
    log("Changing output to new fd: " + std::to_string(fd) + ", see you there, bye!");
  if (fcntl(fd, F_GETFL) == -1) {
    log("file descriptor: " + std::to_string(fd) + " is not open.\n");
    exit(EXIT_FAILURE);
  }
  m_fd = fd;
  m_isatty = isatty(m_fd);
}

void Logger::log(std::string message, bool disable) {
  if (disable)
    return;
  if (m_isatty) {
    std::vector<AnsiAttributes> styles = {AnsiItalic};
    auto noStyles = styles;
    noStyles.emplace(noStyles.begin(), AnsiAttributes::AnsiNo);

    std::cerr << applyStyle(styles) + "DEBUG: " + applyStyle(noStyles) +
                     message + "\n";
  } else {
    std::vector<AnsiAttributes> styles = {AnsiItalic, AnsiUnderline};
    auto noStyles = styles;
    noStyles.emplace(noStyles.begin(), AnsiAttributes::AnsiNo);

    // TODO add date and time here
    std::string formattedMessage =
        applyStyle(styles) + "DEBUG: " + applyStyle(noStyles) + message + "\n";

    write(m_fd, formattedMessage.c_str(), formattedMessage.size());
  }
}

void Logger::logErr(std::string message, bool disable) {
  if (disable)
    return;
  if (m_isatty) {
    std::vector<AnsiAttributes> styles = {AnsiBright};
    auto noStyles = styles;
    noStyles.emplace(noStyles.begin(), AnsiAttributes::AnsiNo);

    std::cerr << applyStyle(styles) + "ERROR: " + applyStyle(noStyles) +
                     message + "\n";
  } else {
    std::vector<AnsiAttributes> styles = {AnsiBright, AnsiUnderline};
    auto noStyles = styles;
    noStyles.emplace(noStyles.begin(), AnsiAttributes::AnsiNo);

    // TODO add date and time here
    std::string formattedMessage =
        applyStyle(styles) + "ERROR: " + applyStyle(noStyles) + message + "\n";

    write(m_fd, formattedMessage.c_str(), formattedMessage.size());
  }
}
} // namespace Log
} // namespace AN
