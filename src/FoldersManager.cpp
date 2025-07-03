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
std::string SocketAddr;

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
  if (!m_stream) {
    // has not yet been set up
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
  m_queue = dispatch_queue_create(nullptr, DISPATCH_QUEUE_SERIAL);
}

FoldersManager::FoldersManager(std::vector<fs::path> folderNames)
    : FoldersManager() {
  addFolders(folderNames);
  // set up fseventstream monitors:
  createEventStream();
}

FoldersManager::~FoldersManager() {
  if (isRunning.load()) {
    stop();
  }
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
  m_serverRunning = true;
  // make enum of recognized signals, separate out server class which dispatches
  // these commands to here, read from the socket
  m_serverSock = socket(AF_UNIX, SOCK_STREAM, 0);
  if (m_serverSock == 1) {
    m_logger.logErr("Unable to open socket.");
    exit(EXIT_FAILURE);
  }

  // fill out local socket address binding
  struct sockaddr_un local;
  // remote filled by accept()
  local.sun_family = AF_UNIX;
  // point us to the socket address
  strcpy(local.sun_path, SocketAddr.c_str());
  // remove reference to file. if open nowhere else, delete it (so reset socket
  // file when starting server)
  std::cout << local.sun_path << "\n";
  local.sun_len = (sizeof(local.sun_family) + strlen(local.sun_path) + 1);

  unlink(local.sun_path);
  // bind socket num to file address
  if (bind(m_serverSock, (struct sockaddr *)&local, local.sun_len) == -1) {
    m_logger.logErr("Unable to bind socket to address: " + SocketAddr + "\n" +
                    strerror(errno));
    exit(EXIT_FAILURE);
  }

  if (listen(m_serverSock, 3) == -1) {
    m_logger.logErr("Unable to set listen.");
    exit(EXIT_FAILURE);
  }

  while (m_serverRunning) {
    m_logger.log("Waiting for connections");

    struct sockaddr_un remote;
    socklen_t remoteLen = sizeof(remote);
    // fill out received client address binding
    m_clientSock =
        accept(m_serverSock, reinterpret_cast<sockaddr *>(&remote), &remoteLen);

    // set socket to be nonblocking
    if (fcntl(m_clientSock, F_SETFL,
              fcntl(m_clientSock, F_GETFL, 0) | O_NONBLOCK) == -1) {
      m_logger.logErr("Unable to set socket to nonblocking");
      exit(EXIT_FAILURE);
    }
    m_logger.log("Received connection" + std::to_string(m_clientSock));

    // now do poll loop waiting for message from pair
    std::array<struct pollfd, 1> pollFds;
    pollFds[0].fd = m_clientSock;
    pollFds[0].events = POLLIN | POLLHUP;
    ;

    m_logger.log("Polling on connections");
    poll(pollFds.data(), 2, -1);

    if (pollFds[0].revents & POLLIN) {
      // TODO split out main thread processing logic here, esp at switch
      m_logger.log("Client message ready to read");
      ServerCommands command; // prepare for reading from socket

      int num = recv(m_clientSock, &command, sizeof(command), 0);
      if (num < 0) {
        m_logger.logErr("Failed at recv");
        exit(EXIT_FAILURE);
      }
      if (num == 0) {
        m_logger.log("Maybe err: received EOF from recv");
      }
      m_logger.log("Received value: " + std::to_string(command));

      handleMessage(m_clientSock, command);
      close(m_clientSock);
    }
  }
}

void FoldersManager::serverStop() {
  // TODO note we are just looping so just tidy up then exit
  close(m_clientSock);
  m_serverRunning = false;
  m_logger.log("Quitting from client request");
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
    std::string msg = "server quitting.\n";
    sendString(fd, msg);
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

FoldersManagerClient::FoldersManagerClient() {}

FoldersManagerClient::~FoldersManagerClient() { disconnect(); }

void FoldersManagerClient::connect() {
  // main gateway to each function, re-establish connection. Makes streaming
  // sockets easy to read to completion vs separating several commands+output
  struct sockaddr_un remoteAddr;
  remoteAddr.sun_family = AF_UNIX;
  strcpy(remoteAddr.sun_path, SocketAddr.c_str());
  remoteAddr.sun_len =
      sizeof(remoteAddr.sun_family) + strlen(remoteAddr.sun_path) + 1;
  // + 1 for null terminator

  if ((m_sock = socket(AF_UNIX, SOCK_STREAM, 0)) == -1) {
    std::cerr << "client socket() call error\n";
    exit(EXIT_FAILURE);
  }

  // need global scope resolver for connect()
  if (::connect(m_sock, reinterpret_cast<sockaddr *>(&remoteAddr),
                remoteAddr.sun_len) == -1) {
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

  std::string out = recvString(m_sock);
  disconnect();
  return out;
}

int FoldersManagerClient::sendCommand(ServerCommands command) {
  int ret = send(m_sock, &command, sizeof(command), 0);
  if (ret == -1) {
    std::cerr << "send() error " << strerror(errno) << "\n";
  }
  return ret;
}

std::string FoldersManagerClient::doServerQuit() {
  // tell server to quit and query response
  connect();

  sendCommand(ServerQuit);

  std::string out = recvString(m_sock);
  disconnect();
  return out;
}

void FoldersManagerClient::disconnect() {
  // don't stop server, but tell it we are done and closing our connection so it
  // waits for someone new
  close(m_sock); // TODO add error or status check
}

}; // namespace AN
