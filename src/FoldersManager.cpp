#include "FoldersManager.hpp"

#include <CoreServices/CoreServices.h>
#include <algorithm>
#include <array>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <mutex>
#include <numeric>
#include <poll.h>
#include <ranges>
#include <span>
#include <string>
#include <sys/poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <thread>
#include <tuple>
#include <unistd.h>
#include <vector>

namespace AN {
namespace fs = std::filesystem;
namespace ranges = std::ranges;

// filename in current directory for socket communication
// constexpr std::string SocketAddrSuffix{"mysocket"};

// const std::string SocketAddr{"/home/aleshapp/mysocket"};
// might not have write access outside parent folder due to apple...
const std::string SocketAddr{
    "/Users/alex/Ext/Code/MusicMonitor/build/mysocket"};

std::condition_variable doScanCV;
std::mutex doScanMutex;
bool doScan;

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

FolderScanner::FolderScanner(fs::path directory) : m_directoryRoot(directory) {
  if (scan() == -1) {
    std::cerr << "Scan failed\n";
    exit(EXIT_FAILURE);
  }
}

int FolderScanner::scan() {
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

bool FolderScanner::isValidExtension(const fs::directory_entry &entry) {
  return std::any_of(m_filetypeFilter.begin(), m_filetypeFilter.end(),
                     [&](const std::string_view filetype) {
                       return filetype == entry.path().extension();
                     });
}

std::vector<fs::path> FolderScanner::getNewFiles() const {
  std::vector<fs::path> outFiles;
  for (auto &f : m_files) {
    outFiles.emplace_back(f.first);
  }
  return outFiles;
}

void callback(ConstFSEventStreamRef stream, void *callbackInfo,
              size_t numEvents, void *evPaths,
              const FSEventStreamEventFlags evFlags[],
              const FSEventStreamEventId evIds[]) {
  {
    std::lock_guard<std::mutex> lock(doScanMutex);
    doScan = true;
  }
  // this needs to come after the lock_guard is released:
  doScanCV.notify_all();
  std::cout << "notified callback\n";
}

void FoldersManager::quitEventStream() {
  if (first) {
    first = false;
    return;
  }

  // still need to update latest log change log so next guy sees it's old news
  m_latestEventId = FSEventStreamGetLatestEventId(m_stream);
  std::cout << "last event id: " << m_latestEventId << "\n";

  std::ofstream outfilelog(m_logFile, std::ios::out | std::ios::trunc);
  if (outfilelog) {
    outfilelog << m_latestEventId;
  }
  // TODO might need to check if currently running in case of double free :/
  FSEventStreamStop(m_stream);
  FSEventStreamInvalidate(m_stream);
  FSEventStreamRelease(m_stream);
}

void FoldersManager::addFolders(std::span<fs::path> folderNames) {
  // update file list, then close and restart event stream
  // add unique elements packed as a tuple
  m_trackedFoldersAndScanners.insert_range(
      folderNames | std::views::transform([](auto f) {
        return std::tuple(f, FolderScanner(f));
      }));

  quitEventStream();
  createEventStream();
}

void FoldersManager::createEventStream() {
  CFMutableArrayRef pathRefs = CFArrayCreateMutable(nullptr, 0, nullptr);
  for (const auto &folderAndScanner : m_trackedFoldersAndScanners) {
    fs::path folderName = folderAndScanner.first;

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

    FSEventStreamSetDispatchQueue(m_stream, m_queue);
    if (!FSEventStreamStart(m_stream)) {
      std::cerr << "Failed to start stream\n";
      exit(EXIT_FAILURE);
    }
  }
}

FoldersManager::FoldersManager() : m_logger(STDOUT_FILENO) {
  m_queue = dispatch_queue_create(nullptr, DISPATCH_QUEUE_CONCURRENT);
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, m_socketKillPair) == -1) {
    m_logger.logErr("socketpair() error");
    exit(EXIT_FAILURE);
  }
}

FoldersManager::FoldersManager(std::vector<fs::path> folderNames)
    : FoldersManager() {
  // m_trackedFolders.insert_range(folderNames);
  // // TODO get folderNames from user input or config
  // // TODO need to separate so can add new folders later...
  // // Looks like we'll need to set up new event stream object...

  // // set up folderscanner handlers:
  // for (const auto &folderName : folderNames) {
  //   m_trackedFoldersScanners.emplace_back(FolderScanner(folderName));
  // }

  addFolders(folderNames);
  // set up fseventstream monitors:
  createEventStream();
}

FoldersManager::~FoldersManager() {
  quitEventStream();
  dispatch_release(m_queue);
}

void FoldersManager::run() {
  // isRunning = true;
  isRunning.store(true);
  // launch a thread
  m_runner = std::thread([this]() {
    while (1) {
      // first wait for pipe/mutex+cv
      std::unique_lock<std::mutex> uniqueLock(doScanMutex);
      // cvSyncFSEventStreamToFolderManager.wait(uniqueLock);
      doScanCV.wait(uniqueLock, []() { return doScan; });
      uniqueLock.unlock(); // wait leaves mutex locked so need to release

      if (!isRunning.load())
        break;

      std::vector<fs::path> filesToProcess;
      // index and get list of all new files:
      for (auto &folderAndScanner : m_trackedFoldersAndScanners) {
        FolderScanner &folderScanner = folderAndScanner.second;
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
}

void FoldersManager::stop() {
  quitThread();
  m_runner.join();
}

void FoldersManager::serverStart() {
  // make enum of recognized signals, separate out server class which dispatches
  // these commands to here, read from the socket
  m_socketId = socket(AF_UNIX, SOCK_STREAM, 0);
  if (m_socketId == 1) {
    m_logger.logErr("Unable to open socket.");
    exit(EXIT_FAILURE);
  }

  // remote filled later by accept(), local filled when creating socket
  struct sockaddr_un remote, local;
  // remote filled by accept()
  local.sun_family = AF_UNIX;
  // point us to the socket address
  strcpy(local.sun_path, SocketAddr.c_str());
  // remove reference to file. if open nowhere else, delete it (so reset socket
  // file when starting server)
  unlink(local.sun_path);

  // bind socket num to file address
  if (bind(m_socketId, (struct sockaddr *)&local,
           (sizeof(local.sun_family) + strlen(SocketAddr.c_str()))) == -1) {
    m_logger.logErr("Unable to bind socket to address: " + SocketAddr);
    exit(EXIT_FAILURE);
  }

  if (listen(m_socketId, 3) == -1) {
    m_logger.logErr("Unable to set listen.");
    exit(EXIT_FAILURE);
  }

  while (1) {
    // move to socket listening instead
    socklen_t remoteLen = sizeof(remote);
    m_logger.log("Waiting for connections");

    // wait until receive client:
    int clientId =
        accept(m_socketId, reinterpret_cast<sockaddr *>(&remote), &remoteLen);
    // set socket to be nonblocking
    if (fcntl(clientId, F_SETFL, fcntl(clientId, F_GETFL, 0) | O_NONBLOCK) ==
        -1) {
      m_logger.logErr("Unable to set socket to nonblocking");
      exit(EXIT_FAILURE);
    }
    m_logger.log("Received connection" + std::to_string(clientId));

    // now do poll loop waiting for message from pair
    std::array<struct pollfd, 2> pollFds;
    pollFds[1].fd = clientId;
    // handle closed connection POLLHUP to
    // not quit but release client
    pollFds[1].events = POLLIN | POLLHUP;
    // listen for kill from socketpair from main
    pollFds[0].fd = m_socketKillPair[1];
    pollFds[0].events = POLLIN;

    m_logger.log("Polling on connections");
    poll(pollFds.data(), 2, -1);

    if (pollFds[0].revents & POLLIN) {
      // received data from kill channel, quit server loop
      m_logger.log("Received socketpair quit signal");
      break;

    } else if (pollFds[1].revents & POLLIN) {
      // TODO split out main thread processing logic here, esp at switch
      m_logger.log("Client message ready to read");
      ServerCommands command; // prepare for reading from socket

      int num = recv(clientId, &command, sizeof(command), 0);
      if (num < 0) {
        m_logger.logErr("Failed at recv");
        exit(EXIT_FAILURE);
      }
      if (num == 0) {
        m_logger.log("Maybe err: received EOF from recv");
      }
      m_logger.log("Received value: " + std::to_string(command));

      handleMessage(clientId, command);
      close(clientId); // TODO add this? Not sure if we also want to terminate
                       // the connection here, but prob
    }
  }
}

void FoldersManager::serverStop() {
  // open socketpair and send message to network thread, call when receive
  // quit command from client
  uint32_t msg = 0; // just send something to trigger ready
  if (send(m_socketKillPair[0], &msg, sizeof(msg), 0) == -1) {
    std::cerr << "Failed to send quit message to server\n";
    exit(EXIT_FAILURE);
  }
}

std::vector<fs::path> FoldersManager::getNewFiles() {
  std::vector<fs::path> newFiles;
  for (auto &folderAndScanner : m_trackedFoldersAndScanners) {
    auto &scanner = folderAndScanner.second;
    auto moreFiles = scanner.getNewFiles();
    newFiles.insert(newFiles.end(), moreFiles.begin(), moreFiles.end());
  }
  return newFiles;
}

void FoldersManager::handleMessage(int fd, ServerCommands command) {
  switch (command) {
  case ServerListFiles: {
    // return a string of all new files

    // for (auto &scanner : m_trackedFoldersScanners) {
    //   std::vector<fs::path> newfiles = scanner.getNewFiles();
    //   for (auto &file : newfiles) {
    //     listFiles = listFiles + "," + file.string();
    //   }
    // }
    auto newFiles = getNewFiles();
    std::string listFiles;
    listFiles = std::accumulate(newFiles.begin(), newFiles.end(), listFiles,
                                [](std::string str, const fs::path path) {
                                  return str.append(path.string() + ",");
                                });
    listFiles.pop_back(); // remove last ','

    sendString(fd, listFiles);
    break;
  }
  case ServerQuit: {
    serverStop();
    break;
  }
  default:
    break;
  }
}

void FoldersManager::quitThread() {
  // send stop to worker thread
  isRunning.store(false);
  {
    // signal scan to break guard
    std::lock_guard<std::mutex> lock(doScanMutex);
    doScan = true;
    doScanCV.notify_all();
  }
}

void sendString(int fd, std::string_view msg) {
  // TODO implement similar loop as recvString in case send doesn't do all bytes
  // (same return value as recv)
  // send header
  uint32_t strsize = msg.length();
  if (send(fd, &strsize, sizeof(strsize), 0) == -1) {
    std::cerr << "Failed to send() string size\n";
  }
  // send body
  if (send(fd, msg.data(), msg.length(), 0) == -1) {
    std::cerr << "Failed to send() string data\n";
  }
}

std::string recvString(int fd) {
  // read header
  uint32_t strsize;
  if (recv(fd, &strsize, sizeof(strsize), 0) != sizeof(strsize)) {
    std::cerr << "Failed to recv() string size\n";
  }
  // read body
  std::vector<char> buffer(strsize);
  size_t charsReceived = 0;
  size_t charsRemaining = strsize;
  int res;
  while (charsRemaining > 0) {
    res = recv(fd, &buffer[charsReceived], charsRemaining, 0);
    if (res == -1) {
      std::cerr << "Failed to recv() string data\n";
    } else if (res == 0) {
      std::cerr
          << "Socket closed connection before completed sending message\n";
    } else {
      // not all data yet received, keep going
      // processed res bytes
      charsReceived += res;
      charsRemaining -= res;
    }
  }
  std::string out(buffer.begin(), buffer.end());
  return out;
}

FoldersManagerClient::FoldersManagerClient() {
  m_remote.sun_family = AF_UNIX;
  strcpy(m_remote.sun_path, SocketAddr.c_str());
  m_remoteSize = sizeof(m_remote.sun_family) + strlen(m_remote.sun_path);
}

FoldersManagerClient::~FoldersManagerClient() { disconnect(); }

void FoldersManagerClient::connect() {
  // main gateway to each function, re-establish connection. Makes streaming
  // sockets easy to read to completion vs separating several commands+output

  if ((m_socketId = socket(AF_UNIX, SOCK_STREAM, 0)) == -1) {
    std::cerr << "client socket() call error\n";
    exit(EXIT_FAILURE);
  }

  // need global scope resolver for connect()
  if (::connect(m_socketId, reinterpret_cast<sockaddr *>(&m_remote),
                m_remoteSize) == -1) {
    std::cerr << "client connect() call error\n";
    exit(EXIT_FAILURE);
  }

  std::cout << "connected\n";
}

std::string FoldersManagerClient::getServerNewFiles() {
  // TODO error handling if empty. When no files I saw total buffer overflow
  // garbage

  // gets a list of all monitored directories
  connect();

  sendCommand(ServerListFiles);
  // ServerCommands value = ServerCommands::ServerListFiles;
  // if (send(m_socketId, &value, sizeof(value), 0) == -1) {
  //   std::cerr << "send() error " << strerror(errno) << "\n";
  // }

  // std::string out;
  // char recvData[100];
  // memset(recvData, 0, sizeof(recvData));
  // // need to read until some sort of end signal from the server... separate
  // from
  // // other commands
  // while (recv(m_socketId, recvData, sizeof(recvData), 0) > 0) {
  //   std::cout << recvData;
  //   out = out + recvData;
  //   memset(recvData, 0, sizeof(recvData)); // TODO unsure maybe need to clear
  //   // std::cerr << "recv() error\n";
  // }
  std::string out = recvString(m_socketId);
  disconnect();
  return out;
}

int FoldersManagerClient::sendCommand(ServerCommands command) {
  int ret = send(m_socketId, &command, sizeof(command), 0);
  if (ret == -1) {
    std::cerr << "send() error " << strerror(errno) << "\n";
  }
  return ret;
}

std::string FoldersManagerClient::doServerQuit() {
  // tell server to quit and query response
  connect();

  sendCommand(ServerQuit);

  std::string out = recvString(m_socketId);
  disconnect();
  return out;
}

void FoldersManagerClient::disconnect() {
  // don't stop server, but tell it we are done and closing our connection so it
  // waits for someone new
  close(m_socketId);         // TODO add error or status check
  unlink(m_remote.sun_path); // remove this reference
}

}; // namespace AN
