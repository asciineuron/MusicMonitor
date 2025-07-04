#pragma once
#include "BackupManager.hpp"
#include "Log.hpp"
#include <CoreServices/CoreServices.h>
#include <filesystem>
#include <memory>
#include <span>
#include <sys/un.h>
#include <thread>
#include <unordered_map>

// AsciiNeuron - limit global vars scope
namespace AN {
namespace fs = std::filesystem;

// might not have write access outside parent folder due to apple...
extern std::string SocketAddr;

// get last modified time from file name
time_t getFileTime(fs::path path);

// checks if checkParent is the initial subset of child i.e. a parent to it
bool isParentDir(const fs::path checkParent, const fs::path child);

class FolderScanner {
  // TODO break down so FSEvents gives which specific sub folders changed. if so
  // only scan those, not whole thing (only needed for subfolder)
public:
  // don't scan yet since blocks callback? maybe actually ok
  // TODO separate out to precheck, do scan wait later
  FolderScanner(fs::path directory);
  FolderScanner(fs::path directory, BackupManager *backupManager);
  ~FolderScanner();

  int scan();
  int scan(const fs::path subdir); // for FSEvents, if subdir is under dir root,
                                   // just scan that part (speedup)
  std::vector<fs::path> getNewFiles() const;
  std::vector<std::pair<fs::path, time_t>> getFilesAndTimes() const; // get all files and their times
  fs::path getRoot() const;

private:
  std::filesystem::path m_directoryRoot;
  enum FileUpdateType { New, Updated, Old };
  std::unordered_map<fs::path, std::pair<FileUpdateType, time_t>>
      m_files;
  std::vector<std::string> m_filetypeFilter{{".flac"}, {".txt"}};
  bool isValidExtension(const fs::directory_entry &entry);
  BackupManager *m_backupManager{}; // Managed by FoldersManager
  // internal function to do actual indexing starting at dir
  int scanDir(const fs::path subdir);
  void restoreContents(); // use BackupManager when first starting up
};

enum ServerCommands {
  ServerListFiles,
  ServerQuit,
  ServerCommandsCount
}; // implement in foldermanager server and separate client

// sends formatted { uint32_t len, char *data } data to valid ready socket at fd
void sendString(int fd, std::string_view str);
// parses data from sendString into valid std::string
std::string recvString(int fd);

class FoldersManager {
public:
  FoldersManager(std::vector<fs::path> folderNames);
  FoldersManager(); // TODO need to refactor so can add more folders iteratively
                    // later
  ~FoldersManager();

  void addFolders(std::span<fs::path> folderNames);

  void run();
  void stop();
  void serverStart();
  void serverStop();

  std::vector<fs::path>
  getNewFiles(); // returns list of new files in last batch

  FSEventStreamEventId getLatestEventId() { return m_latestEventId; }

private:
  // need to handle e.g. ctrl z signal to know to put it in background and write
  // to log instead
  Log::Logger m_logger;
  std::unique_ptr<BackupManager> m_backupManager;

  // for server use, client FoldersManagerClient has own copy
  int m_serverSock;
  int m_clientSock;
  // to start/stop network thread
  bool m_serverRunning{false};

  std::atomic_bool isRunning{false};
  std::thread m_runner{};
  FSEventStreamRef m_stream{nullptr};
  FSEventStreamEventId m_latestEventId;
  dispatch_queue_t m_queue{nullptr};
  // this keeps them unique and easily tracked together:
  std::unordered_map<fs::path, FolderScanner> m_trackedFoldersAndScanners;

  fs::path m_converterExe{"/bin/echo"}; // name/path of conversion executable
  fs::path m_logFile{"musicmonitorbackup"}; // where to load/save latest event id etc

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
  std::unique_ptr<BackupManager> m_backupManager{nullptr};
  Log::Logger m_logger;
  int m_sock;
  void connect();
  void disconnect();
  int sendCommand(ServerCommands command); // convenience wrapper
};

} // namespace AN
