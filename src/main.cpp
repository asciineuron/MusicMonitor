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
#include <getopt.h>
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
  printArgs(argc, argv);
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

  int c;
  // important: '+' enables POSIXLY to stop parsing at non option rather than
  // error
  opterr = 0;
  std::string pArg;
  while ((c = getopt(argc, argv, "cp:")) != -1) {
    switch (c) {
    case 'p':
      std::cout << "trying to connect to server...\n";
      runOnTty = false;
      // optarg guaranteed 0 if optional arg not present
      // if (optarg) {
      pArg = std::string(optarg);
      // }
      break;
    case 'c':
      std::cout << "trying to create a server...\n";
      runOnTty = false;
      runAsServer = true;
      break;
    case '?':
      logger.logErr("Unknown option" + std::string(1, c));
    default:
      printf("got %c\n", c);
      break;
    }
  }
  std::cout << pArg << "\n";
  // launch actual program if we aren't a client
  AN::FoldersManager folderManager;
  bool isClient = !runAsServer && !runOnTty;

  if (!isClient) {
    // if not interacting with server, we are launching the script so need to
    // read
    // input files:
    std::vector<fs::path> paths;
    size_t path_idx = 0;
    for (int i = optind; i < argc; ++i) {
      logger.log("Tracking " + std::string(argv[i]));
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

    folderManager.addFolders(paths);
    folderManager.run();
  }

  // set up user input or server-client socket handling
  if (runOnTty) {
    struct termios termOld, termNew;
    tcgetattr(STDIN_FILENO, &termOld);
    termNew = termOld;
    // turn off icanon from appropriate flag group, and other qol flags
    // disable sig to handle and quit manually
    termNew.c_lflag &= ~(ICANON | ECHO | ISIG);
    termNew.c_iflag &= INLCR;

    bool textLoopRunning = true;
    tcsetattr(STDIN_FILENO, TCSANOW, &termNew); // set immediately
    // main input handling loop
    while (textLoopRunning) {
      char c = getchar();

      if (c == 'q' || c == '\x03') {
        printf("quitting\n\r");
        folderManager.stop();
        textLoopRunning = false;
      } else if (c == 'p') {
        // print list of new files
      }
    }

    tcsetattr(STDIN_FILENO, TCSANOW, &termOld); // disable
  } else {
    // TODO set up server and detach
    // start listening on a socket for commands
    if (runAsServer) {
      folderManager.serverStart();

    } else {
      // TODO use getopt to add client query commands eg 'list' 'add' etc
      logger.log("Connected as client.");
      AN::FoldersManagerClient client;
      // std::string serverList = client.getServerNewFiles();
      // std::cout << "received: " << serverList << "\n";
      if (pArg == "quit") {
        client.doServerQuit();
      }
    }
  }

  return 0;
}

// TODO Note termios.h ICANON mode allows control over realtime input but limits
// buffering etc
// see
// https://stackoverflow.com/questions/1798511/how-to-avoid-pressing-enter-with-getchar-for-reading-a-single-character-only
