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

void printArgs(int argc, char *argv[]) {
  for (int i = 0; i < argc; ++i) {
    std::cout << i << " : " << argv[i] << "\n";
  }
}

void exit_cleanup(int sig) {
  write(2, "caught SIGINT, bye!\n", 21);
  exit(0); // TODO let's figure out how to gradually quit ...
}

int main(int argc, char *argv[]) {
  AN::SocketAddr = fs::temp_directory_path() / "musicmonitorsocket";
  AN::Log::Logger logger(STDOUT_FILENO);
  // logger.log("Hello world!");

  // ignore all signals, later dedicate special thread to handle these
  sigset_t set;
  sigemptyset(&set);
  if (pthread_sigmask(SIG_SETMASK, &set, nullptr) == -1) {
        logger.logErr("pthread_sigmask() error: " + std::string(strerror(errno)));
  }

  // handle sigint (move to handling thread)
  struct sigaction sigact = {exit_cleanup, 0, 0};
  if (sigaction(SIGINT, &sigact, nullptr) == -1) {
    logger.logErr("failed to register sigaction");
    exit(EXIT_FAILURE);
  }
  if (argc == 1) {
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
      logger.log("trying to connect to server...");
      runOnTty = false;
      pArg = std::string(optarg);
      break;
    case 'c':
      logger.log("trying to create a server...");
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
    if (runAsServer) {
      folderManager.serverStart();
      // TODO set up detaching/daemon for server
      // start listening on a socket for commands
    } else {
      // TODO use getopt to add client query commands eg 'list' 'add' etc
      logger.log("Connecting as client.");
      AN::FoldersManagerClient client;
      if (pArg == "list") {
        std::string serverList = client.getServerNewFiles();
        std::cout << "received: " << serverList << "\n";
      } else if (pArg == "quit") {
        std::string response = client.doServerQuit();
        std::cout << "received: " << response << "\n";
      }
    }
  }

  return 0;
}
