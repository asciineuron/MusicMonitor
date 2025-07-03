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
class BackupManager {
public:
  virtual ~BackupManager() = 0;
  // TODO wrap linux handling with inotify here
  virtual FSEventStreamEventId getLastObservedEventId() = 0;
  // is this path the root of some FolderScanner? If not, toss when loading
  virtual bool isMonitoredRoot(fs::path path) = 0;

  // get files and timestamps monitored under given root dir
  virtual std::vector<std::pair<fs::path, time_t>>
  getRootMonitoredFiles(fs::path path) = 0;
};

class JsonManager : public BackupManager {
public:
  JsonManager();

  bool isMonitoredRoot(fs::path path) override;

  std::vector<std::pair<fs::path, time_t>>
  getRootMonitoredFiles(fs::path path) override;

private:
  Json m_json; // loaded from file into json
};

} // namespace AN
