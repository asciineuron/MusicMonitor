#include "FoldersManager.hpp"
#include "BackupManager.hpp"
#include "SettingsManager.hpp"

#include <algorithm>
#include <array>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <numeric>
#include <poll.h>
#include <ranges>
#include <string>
#include <sys/poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <tuple>
#include <unistd.h>
#include <vector>

namespace AN {
namespace fs = std::filesystem;

// filename in temp directory for socket communication
std::string SocketAddr;

std::condition_variable doScanCV;
std::mutex doScanMutex;
bool doScan;

void fileListExecutor(const fs::path &command,
                      std::span<const fs::path> filenames, bool doParallel,
                      bool keep) {
  // void fileListExecutor(const fs::path &command,
  //                       std::set<fs::path> filenames, bool doParallel) {
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
        std::cout << "processing eg" << argv[1] << "\n";
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
    strncpy(argv[0], command.c_str(), commandLen + 1);

    for (int i = 1; i < argc; ++i) {
      argv[i] = static_cast<char *>(
          malloc(sizeof(char) * (strlen(filenames[i - 1].c_str()) + 1)));
      strncpy(argv[i], filenames[i - 1].c_str(),
              strlen(filenames[i - 1].c_str()) + 1);
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

  // finished, delete original file if requested
  if (!keep) {
    for (const fs::path &file : filenames) {
      // TODO replace with rm when done
      std::string cmdStr = "echo " + file.string();
      system(cmdStr.c_str());
    }
  }
}

std::vector<std::pair<fs::path, time_t>>
FolderScanner::getFilesAndTimes() const {
  std::vector<std::pair<fs::path, time_t>> filesAndTimes;
  for (auto &elem : m_files) {
    std::cout << elem.first << ";" << elem.second.second << "\n";
    filesAndTimes.emplace_back(std::pair(elem.first, elem.second.second));
  }
  return filesAndTimes;
}

fs::path FolderScanner::getRoot() const { return m_directoryRoot; }

FolderScanner::FolderScanner(fs::path directory, BackupManager *backupManager)
    : m_directoryRoot(directory), m_backupManager(backupManager) {
  restoreContents();
  scan(); // still need to check for newer files since then in case any files
          // preceeding event id update
}

FolderScanner::FolderScanner(fs::path directory) : m_directoryRoot(directory) {
  restoreContents();
  scan(); // still need to check for newer files since then in case any files
          // preceeding event id update
}

time_t getFileTime(fs::path path) {
  // returns last *access* time
  struct stat attributes;
  stat(path.c_str(), &attributes);
  return attributes.st_atime;
}

bool isParentDir(const fs::path checkParent, const fs::path child) {
  fs::path parent;
  while ((parent = child.parent_path()) != child) {
    // == child when at root, so incorrect if checkParent is root but root
    // parent unlikely
    if (checkParent == parent)
      return true;
  }
  return false;
}

void FolderScanner::restoreContents() {
  if (!m_backupManager)
    return;
  // not yet tracking here, start fresh
  if (!m_backupManager->isMonitoredRoot(m_directoryRoot))
    return;

  auto loadedTimes = m_backupManager->getRootMonitoredFiles(m_directoryRoot);
  m_files.insert_range(loadedTimes | std::views::transform([](auto pathTime) {
                         return std::tuple(
                             pathTime.first,
                             std::tuple(FileUpdateType::Old, pathTime.second));
                       }));
}

int FolderScanner::scanDir(const fs::path subdir) {
  for (const fs::directory_entry &entry :
       fs::recursive_directory_iterator(subdir)) {
    if (!isValidExtension(entry))
      continue;

    FileUpdateType type;
    time_t entryPosixTime = getFileTime(entry.path());
    bool wasTracked = m_files.contains(entry.path());

    auto &updateFile = m_files[entry.path()]; // get or insert in either case...
    if (wasTracked) {
      type = entryPosixTime > updateFile.second ? Updated : Old;
    } else {
      type = New;
    }
    updateFile.first = type;
    updateFile.second = entryPosixTime;
  }
  return 1;
}

int FolderScanner::scan() { return scanDir(m_directoryRoot); }

int FolderScanner::scan(const fs::path subdir) {
  if (!isParentDir(m_directoryRoot, subdir)) {
    return -1;
  }
  return scanDir(subdir);
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
    if (f.second.first == New || f.second.first == Updated) {
      outFiles.emplace_back(f.first);
    }
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
  doScanCV.notify_one();
  std::cout << "notified callback\n";
}

void FoldersManager::quitEventStream() {
  if (!m_stream) {
    // has not yet been set up
    return;
  }

  // still need to update latest log change log so next guy sees it's old news
  m_latestEventId = FSEventStreamGetLatestEventId(m_stream);
  m_logger.log("last event id: " + std::to_string(m_latestEventId) + "\n");

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

  // This prevents creation of unneeded scanners if !contains path compared to
  // fancy range approach
  for (const auto &path : folderNames) {
    if (!m_trackedFoldersAndScanners.contains(path)) {
      m_trackedFoldersAndScanners.emplace(std::tuple(
          path, std::move(FolderScanner(path, m_backupManager.get()))));
    }
  }
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

    m_latestEventId = m_backupManager->getLastObservedEventId();
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

  // convert to absolute file path
  m_logFile = fs::current_path() / m_logFile;
  m_backupManager = std::make_unique<JsonManager>(m_logFile);

  // convert to absolute file path
  m_fileTypeFile = fs::current_path() / m_fileTypeFile;
  loadFileTypes();
}

FoldersManager::FoldersManager(std::vector<fs::path> folderNames)
    : FoldersManager() {
  addFolders(folderNames);
  // set up fseventstream monitors:
  createEventStream();
}

FoldersManager::~FoldersManager() {
  if (m_isRunning.load()) {
    stop();
  }
  quitEventStream();
  dispatch_release(m_queue);

  m_backupManager->getFolderManagerUpdate(*this);
  // query final backup data from managed scanners
  for (auto &foldersAndScanner : m_trackedFoldersAndScanners) {
    m_backupManager->getFolderScannerUpdate(foldersAndScanner.second);
  }
  m_backupManager->updateBackup();
}

void FoldersManager::run() {
  // redirect log to whatever stdin is in case I was daemonized
  // TODO maybe not needed
  m_logger.changeFd(STDIN_FILENO);
  // isRunning = true;
  m_isRunning.store(true);
  // launch a thread
  m_runThread = std::thread([this]() {
    while (1) {
      {
        std::lock_guard<std::mutex> lock(doScanMutex);
        doScan = false;
      }
      // first wait for pipe/mutex+cv
      std::unique_lock<std::mutex> uniqueLock(doScanMutex);
      // cvSyncFSEventStreamToFolderManager.wait(uniqueLock);
      doScanCV.wait(uniqueLock, []() { return doScan; });
      uniqueLock.unlock(); // wait leaves mutex locked so need to release

      if (!m_isRunning.load())
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
      // filter based on settings which cmd and whether to keep
      for (auto &fileSetting : m_fileTypes) {
        // filter files to match this particular extension + settings:
        std::vector<fs::path> filteredFiles;
        filteredFiles.append_range(
            filesToProcess | std::views::filter([&fileSetting](auto f) {
              return f.extension() == fileSetting.extension;
            }));

        std::cout << "executing for extension:" << fileSetting.extension
                  << "\n";
        fileListExecutor(fileSetting.cmd, filteredFiles, false,
                         fileSetting.keep);
      }
    }
    m_logger.log("NOTE I am quitting nicely");
  });
}

void FoldersManager::stop() {
  quitThread();
  m_runThread.join();
}

void FoldersManager::serverStart() {
  m_isServerRunning = true;
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
  local.sun_len = (sizeof(local.sun_family) + strlen(local.sun_path) + 1);

  // delete file if already existing
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

  while (m_isServerRunning) {
    m_logger.log("Waiting for connections");

    struct sockaddr_un remote;
    socklen_t remoteLen = sizeof(remote);
    // fill out received client address binding
    m_clientSock =
        accept(m_serverSock, reinterpret_cast<sockaddr *>(&remote), &remoteLen);
    if (m_clientSock == -1) {
      m_logger.logErr("Error in client accept(): " +
                      std::string(strerror(errno)));
    }
    m_logger.log("Received connection" + std::to_string(m_clientSock));

    // TODO note crashing fcntl when file touched despite no client connected
    // Can maybe ignore
    // set socket to be nonblocking
    int fcntlFlags = fcntl(m_clientSock, F_GETFL, 0);
    if (fcntlFlags == -1) {
      m_logger.logErr("error in fcntlFlags");
    } else if (fcntl(m_clientSock, F_SETFL, fcntlFlags | O_NONBLOCK) == -1) {
      m_logger.logErr("Unable to set socket to nonblocking: " +
                      std::string(strerror(errno)));
      exit(EXIT_FAILURE);
    }

    // now do poll loop waiting for message from pair
    std::array<struct pollfd, 1> pollFds;
    pollFds[0].fd = m_clientSock;
    pollFds[0].events = POLLIN | POLLHUP;

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
  m_isServerRunning = false;
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

void FoldersManager::loadFileTypes() {
  SettingsManager settingsManager(m_fileTypeFile);
  m_fileTypes = settingsManager.getFileSettings();
}

void FoldersManager::quitThread() {
  // send stop to worker thread
  m_isRunning.store(false);
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

FoldersManagerClient::FoldersManagerClient() : m_logger(STDOUT_FILENO) {}

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
    m_logger.logErr("client socket() call error");
    exit(EXIT_FAILURE);
  }

  // need global scope resolver for connect()
  if (::connect(m_sock, reinterpret_cast<sockaddr *>(&remoteAddr),
                remoteAddr.sun_len) == -1) {
    m_logger.logErr("client connect() call error");
    exit(EXIT_FAILURE);
  }

  m_logger.log("connected");
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
    std::string error(strerror(errno));
    m_logger.logErr("send() error " + error + "\n");
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
