#pragma once
#include "FoldersManager.hpp"
#include <filesystem>
#include <nlohmann/json.hpp>

namespace AN {
namespace fs = std::filesystem;
using Json = nlohmann::json; // json is type, so capitalize...

// get the filepaths etc specified by user, and their corresponding options e.g.
// for each folder or *this one->*filetype*, keep or delete processed files?
// what cmd?
// pass in to setting up FoldersManager
// parallel of BackupManager

// schema:
// {
//   "filetype_settings": [
//     {
//       "extension": ".txt",
//       "cmd": "path",
//       "keep": bool,
//     }
//   ]
// }
// TODO also manage list of FOLDERS to watch (independent of filetype)
class SettingsManager {
public:
  SettingsManager(fs::path settingsFile);

  std::vector<FileSettings> getFileSettings();
  // std::vector<fs::path> getFolders();

private:
  Json m_json; // to read settings from (no write)
};

} // namespace AN
