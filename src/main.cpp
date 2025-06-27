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
bool doIndex = true;

void fileListExecutor(std::string command, std::vector<fs::path> filenames,
                      bool doParallel) {
  // do basic fork exec for the command on each filename
  if (doParallel) {
    // fork 'command' for each and every filename
    for (const auto &file : filenames) {
      // first set up argv char**
      int argc = 2; // command and 1 file
      char **argv = static_cast<char **>(malloc(sizeof(char) * (argc + 1)));
      argv[0] = static_cast<char *>(malloc(sizeof(char) * command.size()));
      argv[1] = static_cast<char *>(
          malloc(sizeof(char) * file.string().size()));
      argv[2] = nullptr;

      int ps = fork();
      if (!ps) {
        // child
        execv(command.c_str(), argv);
      } else {
        int ret;
        // later how to parallelize, wrap the loop inside in a thread?
        waitpid(ps, &ret, 0);
        std::cout << "finished waiting for " << ps << " : " << file << "\n";
      }
    }
  } else {
    // all filenames piped to single 'command' fork
    // first set up argv char**
    int argc = 1 + filenames.size();
    char **argv = static_cast<char **>(malloc(sizeof(char) * (argc + 1)));
    argv[0] = static_cast<char *>(malloc(sizeof(char) * command.size()));
    for (int i = 1; i < argc; ++i) {
      argv[i] = static_cast<char *>(
          malloc(sizeof(char) * filenames[i].string().size()));
    }
    argv[argc + 1] = nullptr;
    int ps = fork();
    if (!ps) {
      execv(command.c_str(), argv);
    } else {
    }
  }
}

class FolderScanner {
  // callback gives changed directories
  // for each, this will travel thru all subfiles,
  // see if they are newer than last check, and return list of file names or
  // descriptors for all, or more recent ones
  // use nftw... or fts_open()... or filesystem api!
public:
  FolderScanner(fs::path directory) : m_directoryRoot(directory) { scan(); }

  int scan() {
    for (const fs::directory_entry &entry :
         fs::recursive_directory_iterator(m_directoryRoot)) {
      FileUpdateType type;
      fs::file_time_type entryTime = entry.last_write_time();
      // std::cout << "line: " <<  __LINE__ << " \n";
      bool wasTracked = m_files.contains(entry.path());
      std::pair<FileUpdateType, fs::file_time_type> &updateFile =
          m_files[entry.path()]; // get or insert in either case...
      std::cout << "line: " <<  __LINE__ << " \n";
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
  int m_fd_limit{3};
  std::filesystem::path m_directoryRoot;
  enum FileUpdateType { New, Updated, Old }; // compared against last unix time
  // struct FileStat {
  //   fs::path name;
  //   FileUpdateType status;
  //   time_t timestamp; // can still use posixy stuff here :)
  // };
  // std::vector<FileStat> m_files;
  std::unordered_map<fs::path, std::pair<FileUpdateType, fs::file_time_type>>
      m_files;
};

void callback(ConstFSEventStreamRef stream, void *callbackInfo,
              size_t numEvents, void *evPaths,
              const FSEventStreamEventFlags evFlags[],
              const FSEventStreamEventId evIds[]) {
  // TODO global state here too... let's write to a file and the folderscanner
  // loop can wait on the file change to notify the executor!
  // ACTUALLY don't need to do anything since just here to hang/wait the indexer
  {
    std::lock_guard<std::mutex> doIndexGuard(
        mxSyncFSEventStreamToFolderManager);
    doIndex = true;
  }
  // this needs to come after the lock_guard is released:
  cvSyncFSEventStreamToFolderManager.notify_all();
  std::cout << "line: " << __LINE__ << " \n";
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

    // exit_cleanup(0);
    std::ofstream outfilelog(m_logFile, std::ios::out | std::ios::trunc);
    if (outfilelog) {
      outfilelog << m_latestEventId;
    }
    dispatch_release(m_queue);
    FSEventStreamInvalidate(m_stream);
    FSEventStreamRelease(m_stream);
  }

  void run() {
    // launch a thread
    std::thread thread([this]() {
      while (isRunning) {
        // first wait for pipe/mutex+cv
        std::cout << "line: " << __LINE__ << " \n";
        std::unique_lock<std::mutex> uniqueLock(
            mxSyncFSEventStreamToFolderManager);
        cvSyncFSEventStreamToFolderManager.wait(uniqueLock,
                                                [] { return doIndex; });
        std::cout << "line: " << __LINE__ << " \n";
        std::vector<fs::path> filesToProcess;
        // index and get list of all new files:
        for (FolderScanner &folderScanner : this->m_trackedFoldersScanners) {
          if (folderScanner.scan() == -1) {
            std::cerr << "Error: Failed to complete folder scan.";
            exit(EXIT_FAILURE);
          }
          for (const auto &newFile : folderScanner.getNewFiles()) {
            filesToProcess.push_back(newFile);
          }
        }
        // pass to executor
        std::cout << "line: " << __LINE__ << " \n";
        fileListExecutor(this->m_converterExe, filesToProcess, false);
      }
    });

    {
      std::lock_guard<std::mutex> lock(mxSyncFSEventStreamToFolderManager);
      doIndex = false;
    }
    thread.join();
  }

private:
  FSEventStreamRef m_stream;
  dispatch_queue_t m_queue;
  std::vector<fs::path> m_trackedFolders;
  std::vector<FolderScanner>
      m_trackedFoldersScanners; // can retrieve matching filename from here
  // std::vector<std::string>
  //     m_filesToProcess;       // TODO remove from here, but pass to the
  //                             // converter-executor class instead
  fs::path m_converterExe{"/bin/ls"}; // name/path of conversion executable
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
    // // paths.emplace_back(fs::relative(fs::path(argv[i])));
    // // paths.push_back(fs::path(argv[i]));
    // std::string st = (argv[i]);
    // std::cout << st << "\n";
    // fs::path fp = st;
    // paths.push_back(fp);
    // std::cout << "str: " << paths[i-1].string() << "\n";
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
  AN::isRunning = true;
  folderManager.run();

  return 0;
}

// TODO remove ----- all below
// const char *newestfilelog = "/Volumes/Ext/Code/MusicMonitor/latestEvent.txt";
// FSEventStreamEventId latestEventId;
// char latestEventIdStr[9]; // to hold uint64 plus null

// void exit_cleanup(int sig) {
//   // std::ofstream outfilelog(newestfilelog, std::ios::out |
//   std::ios::trunc);
//   // if (outfilelog) {
//   //   outfilelog << latestEventId;
//   // }
//   // write(0, "caught Ctrl-C and gracefully saving file timestamp log\n",
//   56);
//   // int fd = open(newestfilelog, O_WRONLY | O_TRUNC);
//   // if (fd == -1) {
//   //   write(1, "failed to open timestamp file", 30);
//   // }
//   // write(fd, &latestEventIdStr, sizeof(latestEventIdStr));
//   write(2, "handling exit", 14);
// }

// void callback2(ConstFSEventStreamRef stream, void *callbackInfo,
//                size_t numEvents, void *evPaths,
//                const FSEventStreamEventFlags evFlags[],
//                const FSEventStreamEventId evIds[]) {
//   std::cout << "callback hit!" << std::endl;

//   char **occurredpaths = static_cast<char **>(evPaths);
//   for (size_t i = 0; i < numEvents; i++) {
//     std::cout << occurredpaths[i] << " ; " << evFlags[i] << " ; " << evIds[i]
//               << "\n";
//     FolderScanner fs(occurredpaths[i]);
//   }
//   std::cout << "\n";

//   latestEventId = FSEventStreamGetLatestEventId(
//       stream); // continually save this in case of SIGINT
//   snprintf(latestEventIdStr, 9, "%llu", latestEventId);
// }

// int main(int argc, char *argv[]) { // TODO remove
//   // register signal handling:
//   struct sigaction sigact = {exit_cleanup, 0, 0};
//   if (sigaction(SIGINT, &sigact, nullptr) == -1) {
//     std::cerr << "failed to register sigaction\n";
//     return EXIT_FAILURE;
//   }

//   for (int i = 1; i < argc; ++i) {
//     std::cout << argv[i] << "\n";
//   }
//   if (argc < 2) {
//     std::cerr << "Must supply path to monitor" << "\n";
//     return EXIT_FAILURE;
//   }

//   CFStringRef arg = CFStringCreateWithCString(kCFAllocatorDefault, argv[1],
//                                               kCFStringEncodingUTF8);
//   CFArrayRef paths = CFArrayCreate(NULL, (const void **)&arg, 1, NULL);

//   CFAbsoluteTime latency = 3.0;

//   FSEventStreamEventId sincewhen;
//   {
//     std::ifstream infilelog(newestfilelog, std::ios::in);
//     if (infilelog >> sincewhen) {
//       std::cout << "restoring latest event id: " << sincewhen;
//     } else {
//       sincewhen = kFSEventStreamEventIdSinceNow;
//     }
//   }

//   FSEventStreamRef stream =
//       FSEventStreamCreate(NULL, &callback2, nullptr, paths, sincewhen,
//       latency,
//                           kFSEventStreamCreateFlagNone);

//   dispatch_queue_t queue =
//       dispatch_queue_create(nullptr, DISPATCH_QUEUE_CONCURRENT);

//   FSEventStreamSetDispatchQueue(stream, queue);
//   if (!FSEventStreamStart(stream)) {
//     std::cerr << "Failed to start stream\n";
//     return EXIT_FAILURE;
//   }

//   std::cout << "eventID ; Flag ; Path\n";

//   // CFRunLoopRun(); no need, getline works instead, let's look into portable
//   // poll() or select()

//   std::string instr;
//   while (std::getline(std::cin, instr))
//     ;

//   std::cout << "ending" << std::endl;
//   latestEventId = FSEventStreamGetLatestEventId(stream);
//   std::cout << "last event id: " << latestEventId << "\n";

//   // exit_cleanup(0);
//   std::ofstream outfilelog(newestfilelog, std::ios::out | std::ios::trunc);
//   if (outfilelog) {
//     outfilelog << latestEventId;
//   }
//   dispatch_release(queue);
//   FSEventStreamInvalidate(stream);
//   FSEventStreamRelease(stream);
//   return EXIT_SUCCESS;
// }
