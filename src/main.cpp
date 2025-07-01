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
#include <signal.h>
#include <span>
#include <string>
#include <string_view>
#include <sys/termios.h>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <algorithm>
#include <ranges>
#include <termios.h>
#include <unistd.h>     //STDIN_FILENO
// #include <curses.h>

namespace fs = std::filesystem;
// namespace ranges = std::ranges;

// AsciiNeuron - limit global vars scope
namespace AN {

// some simple data to share from calling main and the run thread
// atomic instead of cv+mutex
struct ThreadAccess {
  std::atomic_bool shouldQuit;
};

ThreadAccess accessManager; // shared state from calling code to start/end :(

std::condition_variable cvSyncFSEventStreamToFolderManager;
std::mutex mxSyncFSEventStreamToFolderManager;
bool isRunning;

void printArgs(int argc, char *argv[]) {
  for (int i = 0; i < argc; ++i) {
    std::cout << i << " : " << argv[i] << "\n";
  }
}

void fileListExecutor(const fs::path &command,
                      std::span<const fs::path> filenames, bool doParallel) {
  const int commandLen = strlen(command.c_str());

  // do basic fork exec for the command on each filename
  if (doParallel) {
    // wrap in a thread loop:
    std::vector<std::thread> threads;
    // fork 'command' for each and every filename
    for (const auto &file : filenames) {
      // first set up argv char**
      // allocate space
      int argc = 2; // command and 1 file
      char **argv = static_cast<char **>(malloc(sizeof(char) * (argc + 1)));
      argv[0] = static_cast<char *>(malloc(sizeof(char) * commandLen + 1));
      argv[1] = static_cast<char *>(
          malloc(sizeof(char) * (strlen(file.c_str()) + 1)));
      // copy contents
      strcpy(argv[0], command.c_str());
      strcpy(argv[1], file.c_str());
      argv[2] = nullptr;

      // printArgs(argc, argv);
      threads.emplace_back(std::thread([&]() {
        int ps = fork();
        if (!ps) {
          // child
          execv(command.c_str(), argv);
        } else {
          int ret;
          if (waitpid(ps, &ret, 0) == -1) {
            std::cerr << "Error waiting for pid: " << ps << "\n";
            exit(EXIT_FAILURE);
          }
        }
      }));
    }

    for (auto &thread : threads) {
      thread.join();
    }

  } else {
    // all filenames piped to single 'command' fork
    // first set up argv char**
    int argc = 1 + filenames.size(); // +1 for 0th ie executable name
    char **argv = static_cast<char **>(
        malloc(sizeof(char) * (argc + 1))); // +1 for final null element

    argv[0] = static_cast<char *>(malloc(sizeof(char) * (commandLen + 1)));
    strcpy(argv[0], command.c_str());

    for (int i = 1; i < argc; ++i) {
      argv[i] = static_cast<char *>(
          malloc(sizeof(char) * (strlen(filenames[i - 1].c_str()) + 1)));
      strcpy(argv[i], filenames[i - 1].c_str());
    }
    argv[argc] = nullptr;
    // printArgs(argc, argv);

    int ps = fork();
    if (!ps) {
      execv(command.c_str(), argv);
    } else {
      int ret;
      if (waitpid(ps, &ret, 0) == -1) {
        std::cerr << "Error waiting for pid: " << ps << "\n";
        exit(EXIT_FAILURE);
      }
    }
  }
}

class FolderScanner {
public:
  // don't scan yet since blocks callback? maybe actually ok
  // TODO separate out to precheck, do scan wait later
  FolderScanner(fs::path directory) : m_directoryRoot(directory) { scan(); }

  int scan() {
    for (const fs::directory_entry &entry :
         fs::recursive_directory_iterator(m_directoryRoot)) {
      if (!isValidExtension(entry))
        continue;

      FileUpdateType type;
      fs::file_time_type entryTime = entry.last_write_time();
      bool wasTracked = m_files.contains(entry.path());
      std::pair<FileUpdateType, fs::file_time_type> &updateFile =
          m_files[entry.path()]; // get or insert in either case...
      if (wasTracked) {
        type = entryTime > updateFile.second ? Updated : Old;
      } else {
        type = New;
      }
      updateFile.first = type;
      updateFile.second = entryTime;
    }
    return 1;
  }

  bool isValidExtension(const fs::directory_entry &entry) {
    return std::any_of(m_filetypeFilter.begin(), m_filetypeFilter.end(),
                       [&](const std::string_view filetype) {
                         return filetype == entry.path().extension();
                       });
  }

  std::vector<fs::path> getNewFiles() {
    std::vector<fs::path> outFiles;
    for (auto &f : m_files) {
      outFiles.emplace_back(f.first);
    }
    return outFiles;
  }

private:
  std::filesystem::path m_directoryRoot;
  enum FileUpdateType { New, Updated, Old };
  std::unordered_map<fs::path, std::pair<FileUpdateType, fs::file_time_type>>
      m_files;
  std::vector<std::string> m_filetypeFilter{{".flac"}, {".txt"}};
};

void callback(ConstFSEventStreamRef stream, void *callbackInfo,
              size_t numEvents, void *evPaths,
              const FSEventStreamEventFlags evFlags[],
              const FSEventStreamEventId evIds[]) {
  // this needs to come after the lock_guard is released:
  cvSyncFSEventStreamToFolderManager.notify_all();
  // accessManager.shouldQuit.notify_all();
  std::cout << "notified at line: " << __LINE__ << " \n";
}

class FoldersManager {
public:
  FoldersManager(std::vector<fs::path> folderNames)
      : m_trackedFolders(folderNames) {
    // set up folderscanner handlers:
    for (const auto &folderName : folderNames) {
      m_trackedFoldersScanners.emplace_back(FolderScanner(folderName));
    }

    // set up fseventstream monitors:
    CFMutableArrayRef pathRefs = CFArrayCreateMutable(nullptr, 0, nullptr);
    for (const std::string &folderName : folderNames) {
      CFStringRef arg = CFStringCreateWithCString(
          kCFAllocatorDefault, folderName.c_str(), kCFStringEncodingUTF8);

      CFArrayRef paths = CFArrayCreate(NULL, (const void **)&arg, 1, NULL);

      CFArrayAppendArray(pathRefs, paths,
                         CFRangeMake(0, 1)); // check rangemake second argument,
                                             // 1 elem or length of string data?
      CFAbsoluteTime latency = 3.0;          // TODO control later?
      {                                      // scope infilelog
        std::ifstream infilelog(m_logFile, std::ios::in);
        if (infilelog >> m_latestEventId) {
          std::cout << "restoring latest event id: " << m_latestEventId;
        } else {
          std::cout << "starting timestamp now \n";
          m_latestEventId = kFSEventStreamEventIdSinceNow;
        }
      }
      m_stream =
          FSEventStreamCreate(NULL, &callback, nullptr, paths, m_latestEventId,
                              latency, kFSEventStreamCreateFlagNone);

      m_queue = dispatch_queue_create(nullptr, DISPATCH_QUEUE_CONCURRENT);

      FSEventStreamSetDispatchQueue(m_stream, m_queue);
      if (!FSEventStreamStart(m_stream)) {
        std::cerr << "Failed to start stream\n";
        exit(EXIT_FAILURE);
      }
    }
  } // get folderNames from user input or config

  ~FoldersManager() {
    m_latestEventId = FSEventStreamGetLatestEventId(m_stream);
    std::cout << "last event id: " << m_latestEventId << "\n";

    std::ofstream outfilelog(m_logFile, std::ios::out | std::ios::trunc);
    if (outfilelog) {
      outfilelog << m_latestEventId;
    }
    dispatch_release(m_queue);
    FSEventStreamInvalidate(m_stream);
    FSEventStreamRelease(m_stream);
  }

  void run() {
    isRunning = true;
    // launch a thread
    m_runner = std::thread([this]() {
      while (1) {
        {
          std::lock_guard<std::mutex> lock(mxSyncFSEventStreamToFolderManager);
          if (!isRunning)
            break;
        }

        // first wait for pipe/mutex+cv
        std::unique_lock<std::mutex> uniqueLock(
            mxSyncFSEventStreamToFolderManager);
        cvSyncFSEventStreamToFolderManager.wait(uniqueLock);
        uniqueLock.unlock(); // wait leaves mutex locked so need to release
        // cvSyncFSEventStreamToFolderManager.wait(uniqueLock, [](){return accessManager.shouldQuit.load();});

        if (accessManager.shouldQuit.load() == true)
          break;

        std::vector<fs::path> filesToProcess;
        // index and get list of all new files:
        for (FolderScanner &folderScanner : this->m_trackedFoldersScanners) {
          if (folderScanner.scan() == -1) {
            std::cerr << "Error: Failed to complete folder scan.";
            exit(EXIT_FAILURE);
          }
          for (const auto &newFile : folderScanner.getNewFiles()) {
            std::cout << newFile << "\n";
            filesToProcess.push_back(newFile);
          }
        }

        // pass to executor
        fileListExecutor(this->m_converterExe, filesToProcess, false);
      }
      std::cout << "NOTE I am quitting nicely\n";
    });

    m_runner.join();
  }

private:
  std::thread m_runner;
  FSEventStreamRef m_stream;
  dispatch_queue_t m_queue;
  std::vector<fs::path> m_trackedFolders;
  std::vector<FolderScanner>
      m_trackedFoldersScanners; // can retrieve matching filename from here
  // fs::path m_converterExe{"/bin/ls"}; // name/path of conversion executable
  fs::path m_converterExe{"/bin/echo"}; // name/path of conversion executable
  FSEventStreamEventId m_latestEventId;
  std::string m_logFile; // where to load/save latest event id etc
};
}; // namespace AN

void exit_cleanup(int sig) {
  AN::isRunning = false;
  write(2, "caught SIGINT, bye!\n", 21);
  exit(0); // TODO let's figure out how to gradually quit next...
}

int main(int argc, char *argv[]) {
  // TODO split into pseudo client server. Check socket if running, if so client
  // else server. use to query stats etc

  struct sigaction sigact = {exit_cleanup, 0, 0};
  if (sigaction(SIGINT, &sigact, nullptr) == -1) {
    std::cerr << "failed to register sigaction\n";
    exit(EXIT_FAILURE);
  }
  if (argc == 1) {
    std::cerr << "Need to specify one or more paths to monitor.\n";
    exit(EXIT_FAILURE);
  }
  // for (int i = 0; i < argc; i++) {
  //   std::cout << argv[i] << "\n";
  // }

  std::vector<fs::path> paths;
  for (int i = 1; i < argc; ++i) {
    paths.emplace_back(argv[i]);
    std::error_code ec;
    if (!fs::is_directory(paths[i - 1], ec)) {
      std::cerr << "Input '" << paths[i - 1]
                << "' is not a valid directory path\n";
      std::cerr << "Error: " << ec.value() << ", " << ec.message() << "\n";
      exit(EXIT_FAILURE);
    }
  }

  AN::accessManager.shouldQuit.store(false);

  std::thread folderManagerThread([&]() {
    AN::FoldersManager folderManager(paths);
    folderManager.run();
  });

  // set up user input or server-client socket handling
  // char input;
  // while (std::cin >> input  ) {
  //   std::cout << "you said: " << input << " ";
  // }
  // NCURSES
  // filter(); // don't clear the terminal when starting
  // initscr();
  // cbreak(); // don't wait for enter to read user input
  // char input;
  // while (1) {
  //   input = getch();
  //   int x, y;
  //   getyx(stdscr, y, x);

  //   mvprintw(y+1, x, "you said: %c\n", input);
  // }
  // printw("hi");

  struct termios termOld, termNew;
  tcgetattr(STDIN_FILENO, &termOld);
  termNew = termOld;
  // turn off icanon from appropriate flag group, and other qol flags
  termNew.c_lflag &= ~(ICANON | ECHO);

  termNew.c_iflag &= INLCR; //OPOST;//ONLCR;//INLCR;

  // note need to get out of this mode to do serious printing... instead just
  // loop entering this mode until command, then process normally
  // nope still problematic
  bool textLoopRunning = true;
  tcsetattr(STDIN_FILENO, TCSANOW, &termNew); // set immediately
  while (textLoopRunning) {
    char c = getchar();
    // tcsetattr(STDIN_FILENO, TCSANOW, &termOld); // disable
    // printf("you said: %c\n\r", c);
    if (c == 'q') {
      printf("quitting\n\r");
      // threadAccess.shouldQuit = true;
      // threadAccess.shouldQuit.notify_all();
      AN::accessManager.shouldQuit.store(true);
      AN::cvSyncFSEventStreamToFolderManager.notify_all();
      textLoopRunning = false;
    }
  }
  tcsetattr(STDIN_FILENO, TCSANOW, &termOld); // disable

  // still need 2 ctrl-c to fully quit...

  folderManagerThread.join();
  return 0;
}

// TODO Note termios.h ICANON mode allows control over realtime input but limits
// buffering etc
// see https://stackoverflow.com/questions/1798511/how-to-avoid-pressing-enter-with-getchar-for-reading-a-single-character-only
