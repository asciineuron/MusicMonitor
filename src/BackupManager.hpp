#pragma once
#include <CoreServices/CoreServices.h>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <vector>

namespace AN {
namespace fs = std::filesystem;
using Json = nlohmann::json; // json is type, so capitalize...

// TODO eg json format:
/*
{
  "lasteventid": num,
  "folder_scan_list": [
    { // NOTE each of these is a single FolderScanner
      "folder_root": "str",
      "paths_and_times": [
        {
          "path": "str",
          "time": num,
        },
      ,...
      ]
    } FolderScanner
  ]
}
*/

// manage the text, json, sqlite etc. representation of the state backup, for
// properly loading only unprocessed files in FolderScanner, and latest folder
// event id for FoldersManager
// want to resume monitoring all folders when quit, unless list cleared by
// command to server

class FolderScanner;
class FoldersManager;

class BackupManager {
public:
  virtual ~BackupManager() {};
  // TODO wrap linux handling with inotify here
  virtual FSEventStreamEventId getLastObservedEventId() = 0;
  // is this path the root of some FolderScanner? If not, toss when loading
  virtual bool isMonitoredRoot(fs::path path) = 0;

  // get files and timestamps monitored under given root dir
  virtual std::vector<std::pair<fs::path, time_t>>
  getRootMonitoredFiles(fs::path path) = 0;

  // query new folders to add to me
  virtual void getFolderManagerUpdate(FoldersManager &manager) = 0;
  virtual void getFolderScannerUpdate(FolderScanner &scanner) = 0;
  virtual void updateBackup() = 0;
};

class JsonManager : public BackupManager {
public:
  JsonManager() {};
  JsonManager(fs::path backupFile);
  ~JsonManager() {};

  FSEventStreamEventId getLastObservedEventId() override;

  bool isMonitoredRoot(fs::path path) override;

  std::vector<std::pair<fs::path, time_t>>
  getRootMonitoredFiles(fs::path path) override;

  void getFolderManagerUpdate(FoldersManager &manager) override;
  void getFolderScannerUpdate(FolderScanner &scanner) override;
  void updateBackup() override;

private:
  fs::path m_backupFile{}; // file to source from/to
  // loaded from file into json (keep separate from output for backup/crash)
  Json m_jsonIn{};
  Json m_jsonOut{};
};

} // namespace AN
