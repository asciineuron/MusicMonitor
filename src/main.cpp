#include "FoldersManager.hpp"
#include "Log.hpp"
#include <CoreServices/CoreServices.h>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <ftw.h>
#include <iostream>
#include <mutex>
#include <poll.h>
#include <ranges>
#include <signal.h>
#include <span>
#include <string>
#include <string_view>
#include <sys/termios.h>
#include <system_error>
#include <termios.h>
#include <thread>
#include <unistd.h> //STDIN_FILENO
#include <unordered_map>

namespace fs = std::filesystem;
// namespace ranges = std::ranges;

// some simple data to share from calling main and the run thread
// atomic instead of cv+mutex
// struct ThreadAccess {
//   // std::atomic_bool shouldQuit;
// };

// ThreadAccess accessManager; // shared state from calling code to start/end :(

void printArgs(int argc, char *argv[]) {
  for (int i = 0; i < argc; ++i) {
    std::cout << i << " : " << argv[i] << "\n";
  }
}

void exit_cleanup(int sig) {
  // AN::isRunning = false;
  write(2, "caught SIGINT, bye!\n", 21);
  exit(0); // TODO let's figure out how to gradually quit next...
}

int main(int argc, char *argv[]) {
  // AN::Log::printFmt("Hello world!", {AN::Log::AnsiUnderline});
  AN::Log::Logger logger(STDOUT_FILENO);
  logger.log("Hello world!");
  // TODO split into pseudo client server. Check socket if running, if so client
  // else server. use to query stats etc

  struct sigaction sigact = {exit_cleanup, 0, 0};
  if (sigaction(SIGINT, &sigact, nullptr) == -1) {
    logger.logErr("failed to register sigaction");
    exit(EXIT_FAILURE);
  }
  if (argc == 1) {
    std::cerr << "Need to specify one or more paths to monitor.\n";
    logger.logErr("Need to specify one or more paths to monitor");
    exit(EXIT_FAILURE);
  }

  // direct user interaction, no sockets, else client to server
  bool runOnTty = true;
  // if not at tty, run as server or client
  bool runAsServer = false; // if not at tty, am I the server or client?

  char c;
  // important: '+' enables POSIXLY to stop parsing at non option rather than
  // error
  while ((c = getopt(argc, argv, "p:") != -1)) {
    switch (c) {
    case 'p':
      std::cout << "trying to connect to server...\n";
      runOnTty = false;
      break;
    case 'c':
      std::cout << "trying to create a server...\n";
      runOnTty = false;
      runAsServer = true;
      break;
    default:
      break;
    }
  }

  // if not interacting with server, we are launching the script so need to read
  // input files:
  std::vector<fs::path> paths;
  size_t path_idx = 0;
  for (int i = optind; i < argc; ++i) {
    paths.emplace_back(fs::path(argv[i]));
    std::error_code ec;
    if (!fs::is_directory(paths[path_idx], ec)) {
      std::cerr << "Input '" << paths[path_idx]
                << "' is not a valid directory path\n";
      std::cerr << "Error: " << ec.value() << ", " << ec.message() << "\n";
      exit(EXIT_FAILURE);
    }
    ++path_idx;
  }

  // launch actual program
  AN::FoldersManager folderManager(paths);
  folderManager.run();

  // set up user input or server-client socket handling
  if (runOnTty) {
    struct termios termOld, termNew;
    tcgetattr(STDIN_FILENO, &termOld);
    termNew = termOld;
    // turn off icanon from appropriate flag group, and other qol flags
    // disable sig to handle and quit manually
    termNew.c_lflag &= ~(ICANON | ECHO | ISIG);

    termNew.c_iflag &= INLCR; // OPOST;//ONLCR;//INLCR;

    bool textLoopRunning = true;
    tcsetattr(STDIN_FILENO, TCSANOW, &termNew); // set immediately
    // main input handling loop
    while (textLoopRunning) {
      char c = getchar();
      if (c == 'q' || c == '\x03') {
        printf("quitting\n\r");
        folderManager.stop();
        textLoopRunning = false;
      }
    }
    tcsetattr(STDIN_FILENO, TCSANOW, &termOld); // disable
  } else {
    // TODO set up server and detach
    // start listening on a socket for commands
    if (runAsServer) {
      if (folderManager.serverStart() == -1) {
        logger.logErr("Failed to start server");
        exit(EXIT_FAILURE);
      } else {
        // set up client, basically just to receive info messages from server
      }
    }

  }

  return 0;
}

// TODO Note termios.h ICANON mode allows control over realtime input but limits
// buffering etc
// see
// https://stackoverflow.com/questions/1798511/how-to-avoid-pressing-enter-with-getchar-for-reading-a-single-character-only
