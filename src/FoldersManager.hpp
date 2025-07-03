#pragma once
#include "Log.hpp"
#include <CoreServices/CoreServices.h>
#include <filesystem>
#include <thread>
#include <sys/un.h>
#include <unordered_map>
#include <unordered_set>

// AsciiNeuron - limit global vars scope
namespace AN {
namespace fs = std::filesystem;

// const std::string SocketAddr{"/home/aleshapp/mysocket"};
// might not have write access outside parent folder due to apple...
extern std::string SocketAddr;

class FolderScanner {
public:
  // don't scan yet since blocks callback? maybe actually ok
  // TODO separate out to precheck, do scan wait later
  FolderScanner(fs::path directory);

  int scan();
  std::vector<fs::path> getNewFiles() const;

private:
  std::filesystem::path m_directoryRoot;
  enum FileUpdateType { New, Updated, Old };
  std::unordered_map<fs::path, std::pair<FileUpdateType, fs::file_time_type>>
      m_files;
  std::vector<std::string> m_filetypeFilter{{".flac"}, {".txt"}};
  bool isValidExtension(const fs::directory_entry &entry);
};

enum ServerCommands { ServerListFiles, ServerQuit, ServerCommandsCount }; // implement in foldermanager server and separate client

// sends formatted { uint32_t len, char *data } data to valid ready socket at fd
void sendString(int fd, std::string_view str);
// parses data from sendString into valid std::string
std::string recvString(int fd);

class FoldersManager {
public:
  FoldersManager(std::vector<fs::path> folderNames);
  FoldersManager(); // TODO need to refactor so can add more folders iteratively later
  ~FoldersManager();

  void addFolders(std::span<fs::path> folderNames);

  void run();
  void stop();
  void serverStart();
  void serverStop();

  std::vector<fs::path> getNewFiles(); // returns list of new files in last batch

private:
  // need to handle e.g. ctrl z signal to know to put it in background and write to log instead
  Log::Logger m_logger;

  // for server use, not needed for client
  int m_socketId;
  int m_clientId;
  // to start/stop network thread
  // TODO still keep for graceful local kill maybe, otherwise just exit(success) from quit message...
  bool m_serverRunning; // maybe instead of kill pair, not needed
  // filename in current dir for socket
  // network thread to listen for and handle connections
  // std::thread m_socketThread;
  // fs::path m_socketAddrPrefix{}; // TODO improve this

  std::string m_logAddrPrefix{"mylog"}; // append to current working directory
  std::atomic_bool isRunning;
  std::thread m_runner;
  FSEventStreamRef m_stream{nullptr};
  dispatch_queue_t m_queue{nullptr};
  // std::vector<fs::path> m_trackedFolders;
  std::unordered_map<fs::path, FolderScanner> m_trackedFoldersAndScanners;
  // std::unordered_set<fs::path> m_trackedFolders;
  // std::vector<FolderScanner>
  //     m_trackedFoldersScanners; // can retrieve matching filename from here
  fs::path m_converterExe{"/bin/ls"}; // name/path of conversion executable
  FSEventStreamEventId m_latestEventId;
  std::string m_logFile; // where to load/save latest event id etc

  void quitThread();
  // thread will be waiting for read(), handle and process it here depending on
  // the specific ServerCommand
  // fd is socket of connection to reply to
  void handleMessage(int fd, ServerCommands command);
  void quitEventStream();
  void createEventStream();
};

class FoldersManagerClient {
public:
  FoldersManagerClient();
  ~FoldersManagerClient();

  std::string getServerNewFiles();
  std::string doServerQuit();

private:
  struct sockaddr_un m_remote{};
  int m_socketId{};
  // void clearSocket(); // socket has to be created for each connection, not
  // reused
  void connect();
  void disconnect();
  int sendCommand(ServerCommands command); // convenience wrapper
};

} // namespace AN
