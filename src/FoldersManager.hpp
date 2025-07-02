#pragma once
#include "Log.hpp"
#include <CoreServices/CoreServices.h>
#include <filesystem>
#include <thread>
#include <sys/un.h>

// AsciiNeuron - limit global vars scope
namespace AN {
namespace fs = std::filesystem;

class FolderScanner {
public:
  // don't scan yet since blocks callback? maybe actually ok
  // TODO separate out to precheck, do scan wait later
  FolderScanner(fs::path directory);

  int scan();
  std::vector<fs::path> getNewFiles();

private:
  std::filesystem::path m_directoryRoot;
  enum FileUpdateType { New, Updated, Old };
  std::unordered_map<fs::path, std::pair<FileUpdateType, fs::file_time_type>>
      m_files;
  std::vector<std::string> m_filetypeFilter{{".flac"}, {".txt"}};
  bool isValidExtension(const fs::directory_entry &entry);
};

enum ServerCommands { ServerListFiles, ServerCommandsCount }; // implement in foldermanager server and separate client

// sends formatted { uint32_t len, char *data } data to valid ready socket at fd
void sendString(int fd, std::string_view str);
// parses data from sendString into valid std::string
std::string recvString(int fd);

class FoldersManager {
public:
  FoldersManager(std::vector<fs::path> folderNames);
  ~FoldersManager();

  void run();
  void stop();
  void serverStart();
  void serverStop();

private:
  // need to handle e.g. ctrl z signal to know to put it in background and write to log instead
  Log::Logger m_logger;

  // for server use, not needed for client
  int m_socketId;
  // to start/stop network thread
  int m_socketKillPair[2];
  // filename in current dir for socket
  // network thread to listen for and handle connections
  // std::thread m_socketThread;
  // fs::path m_socketAddrPrefix{}; // TODO improve this

  std::string m_logAddrPrefix{"mylog"}; // append to current working directory
  std::atomic_bool isRunning;
  std::thread m_runner;
  FSEventStreamRef m_stream;
  dispatch_queue_t m_queue;
  std::vector<fs::path> m_trackedFolders;
  std::vector<FolderScanner>
      m_trackedFoldersScanners; // can retrieve matching filename from here
  fs::path m_converterExe{"/bin/ls"}; // name/path of conversion executable
  FSEventStreamEventId m_latestEventId;
  std::string m_logFile; // where to load/save latest event id etc

  void quitThread();
  // void handleSignal(); // thread will be waiting for read(), handle and process it here
};

class FoldersManagerClient {
public:
  FoldersManagerClient();

  std::string getServerListFiles();

  void connect();

  void disconnect();

private:
  struct sockaddr_un remote{};
  int m_socketId{};
  int m_remoteSize{};
};

} // namespace AN
