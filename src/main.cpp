#include <CoreServices/CoreServices.h>
#include <cstdlib>
#include <signal.h>
#include <fstream>
#include <ftw.h>
#include <iostream>
#include <poll.h>
#include <cstring>

const char *newestfilelog = "/Volumes/Ext/Code/MusicMonitor/latestEvent.txt";
FSEventStreamEventId latestEventId;
char latestEventIdStr[9]; // to hold uint64 plus null

void exit_cleanup(int sig) {
  // std::ofstream outfilelog(newestfilelog, std::ios::out | std::ios::trunc);
  // if (outfilelog) {
  //   outfilelog << latestEventId;
  // }
  write(0, "caught Ctrl-C and gracefully saving file timestamp log\n", 56);
  int fd = open(newestfilelog, O_WRONLY | O_TRUNC);
  if (fd == -1) {
    write(1, "failed to open timestamp file", 30);
  }
  write(fd, &latestEventIdStr, sizeof(latestEventIdStr));
}

const char *ftwstr[FTW_SLN + 1] = {"File.",
                                   "Directory.",
                                   "Directory without read permission.",
                                   "Directory with subdirectories visited.",
                                   "Unknown type; stat() failed.",
                                   "Symbolic link.",
                                   "Sym link that names a nonexistent file."};

class FolderScan {
  // callback gives changed directories
  // for each, this will travel thru all subfiles,
  // see if they are newer than last check, and return list of file names or
  // descriptors for all, or more recent ones
  // use nftw
public:
  FolderScan(char *directory) : m_directory(directory) {
    scan();
  }

  // no 4th flags:
  void scan() {
    nftw(m_directory.c_str(), &FolderScan::nftwHandle, m_fd_limit, 0);
    std::cout << "\n";
  }

private:
  int m_fd_limit{3};
  std::string m_directory;
  std::vector<std::string> m_allfiles;
  std::vector<std::string> m_newfiles;
  static int nftwHandle(const char *path, const struct stat *st, int type,
                        struct FTW *ftw) {
    // need static to pass to nftw
    // std::cout << "handle path: " << path << " and type: " << ftwstr[type] <<
    // " "; return 0;
    std::cout << path << ";";
    return 0;
  }
};

void callback(ConstFSEventStreamRef stream, void *callbackInfo,
              size_t numEvents, void *evPaths,
              const FSEventStreamEventFlags evFlags[],
              const FSEventStreamEventId evIds[]) {
  std::cout << "callback hit!" << std::endl;

  char **occurredpaths = static_cast<char **>(evPaths);
  for (size_t i = 0; i < numEvents; i++) {
    std::cout << occurredpaths[i] << " ; " << evFlags[i] << " ; " << evIds[i]
              << "\n";
    FolderScan fs(occurredpaths[i]);
  }
  std::cout << "\n";

  latestEventId = FSEventStreamGetLatestEventId(
      stream); // continually save this in case of SIGINT
  snprintf(latestEventIdStr, 9, "%llu", latestEventId);
}

int main(int argc, char *argv[]) {
  // register signal handling:
  struct sigaction sigact = {exit_cleanup, 0, 0};
  if (sigaction(SIGINT, &sigact, nullptr) == -1) {
    std::cerr << "failed to register sigaction\n";
    return EXIT_FAILURE;
  }

  for (int i = 1; i < argc; ++i) {
    std::cout << argv[i] << "\n";
  }
  if (argc < 2) {
    std::cerr << "Must supply path to monitor" << "\n";
    return EXIT_FAILURE;
  }

  CFStringRef arg = CFStringCreateWithCString(kCFAllocatorDefault, argv[1],
                                              kCFStringEncodingUTF8);
  CFArrayRef paths = CFArrayCreate(NULL, (const void **)&arg, 1, NULL);

  CFAbsoluteTime latency = 3.0;

  FSEventStreamEventId sincewhen;
  {
    std::ifstream infilelog(newestfilelog, std::ios::in);
    if (infilelog >> sincewhen) {
      std::cout << "restoring latest event id: " << sincewhen;
    } else {
      sincewhen = kFSEventStreamEventIdSinceNow;
    }
  }
  FSEventStreamRef stream =
      FSEventStreamCreate(NULL, &callback, nullptr, paths, sincewhen, latency,
                          kFSEventStreamCreateFlagNone);

  dispatch_queue_t queue =
      dispatch_queue_create(nullptr, DISPATCH_QUEUE_CONCURRENT);

  FSEventStreamSetDispatchQueue(stream, queue);
  if (!FSEventStreamStart(stream)) {
    std::cerr << "Failed to start stream\n";
    return EXIT_FAILURE;
  }

  std::cout << "eventID ; Flag ; Path\n";

  // CFRunLoopRun(); no need, getline works instead, let's look into portable
  // poll() or select()

  std::string instr;
  while (std::getline(std::cin, instr))
    ;

  std::cout << "ending" << std::endl;
  //latestEventId = FSEventStreamGetLatestEventId(stream);
  std::cout << "last event id: " << latestEventId << "\n";

  // exit_cleanup(0);

  dispatch_release(queue);
  FSEventStreamInvalidate(stream);
  FSEventStreamRelease(stream);
  return EXIT_SUCCESS;
}
