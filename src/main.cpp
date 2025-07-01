#include <CoreServices/CoreServices.h>
#include <chrono>
#include <condition_variable>
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
#include <system_error>
#include <thread>
#include <unordered_map>
namespace fs = std::filesystem;

// AsciiNeuron - limit global vars scope
namespace AN {
std::condition_variable cvSyncFSEventStreamToFolderManager;
std::mutex mxSyncFSEventStreamToFolderManager;
bool isRunning;

void printArgs(int argc, char *argv[]) {
  for (int i = 0; i < argc; ++i) {
    std::cout << i << " : " << argv[i] << "\n";
  }
}

void fileListExecutor(const fs::path &command, std::span<const fs::path> filenames,
                      bool doParallel) {
  std::cout << command.c_str() << ", " << command.string().length() << "\n";
  const int commandLen = strlen(command.c_str());
  // do basic fork exec for the command on each filename
  if (doParallel) {
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

      int ps = fork();
      if (!ps) {
        // child
        execv(command.c_str(), argv);
      } else {
        int ret;
        // later how to parallelize, wrap the loop inside in a thread?
        waitpid(ps, &ret, 0);
        std::cout << "Finished waiting for " << ps << " : " << file << "\n";
      }
    }
  } else {
    // all filenames piped to single 'command' fork
    // first set up argv char**
    int argc = 1 + filenames.size(); // +1 for 0th ie executable name
    char **argv = static_cast<char **>(malloc(sizeof(char) * (argc + 1))); // +1 for final null element

    argv[0] =
        static_cast<char *>(malloc(sizeof(char) * (commandLen+1)));
    strcpy(argv[0], command.c_str());

    for (int i = 1; i < argc; ++i) {
      argv[i] = static_cast<char *>(
          malloc(sizeof(char) * (strlen(filenames[i-1].c_str())+1)));
      strcpy(argv[i], filenames[i-1].c_str());
    }
    argv[argc] = nullptr;
    // printArgs(argc, argv);

    int ps = fork();
    if (!ps) {
      execv(command.c_str(), argv);
    } else {
      int ret;
      waitpid(ps, &ret, 0);
      std::cout << "Finished waiting for " << ps << "\n";
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
};

void callback(ConstFSEventStreamRef stream, void *callbackInfo,
              size_t numEvents, void *evPaths,
              const FSEventStreamEventFlags evFlags[],
              const FSEventStreamEventId evIds[]) {
  // this needs to come after the lock_guard is released:
  cvSyncFSEventStreamToFolderManager.notify_all();
  std::cout << "notified at line: " << __LINE__ << " \n";
}

class FoldersManager {
public:
  FoldersManager(std::vector<fs::path> folderNames)
      : m_trackedFolders(folderNames) {
    // set upfolderscanner handlers:
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
    std::thread thread([this]() {
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
        uniqueLock.unlock(); // wait locks mutex so need to release

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
        fileListExecutor(this->m_converterExe, filesToProcess, true);
      }
    });

    thread.join();
  }

private:
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
  for (int i = 0; i < argc; i++) {
    std::cout << argv[i] << "\n";
  }

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

  AN::FoldersManager folderManager(paths);
  folderManager.run();

  return 0;
}
