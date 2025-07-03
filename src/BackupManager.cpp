#include "BackupManager.hpp"

namespace AN {

bool JsonManager::isMonitoredRoot(fs::path path) {
  for (const Json folderScanner : m_json["folder_scan_list"]) {
    if (folderScanner["folder_root"] == path.string()) {
      return true;
    }
  }
  return false;
}

std::vector<std::pair<fs::path, time_t>>
JsonManager::getRootMonitoredFiles(fs::path path) {
  std::vector<std::pair<fs::path, time_t>> pathsAndTimes;

  for (const Json &folderScanner : m_json["folder_scan_list"]) {
    if (folderScanner["folder_root"] != path.string())
      continue;

    for (const Json &jPathsAndTimes : folderScanner["paths_and_times"]) {
      fs::path path(jPathsAndTimes["path"]);
      time_t time(std::stoi(static_cast<std::string>(jPathsAndTimes["time"])));

      pathsAndTimes.emplace_back(std::tuple(path, time));
    }
  }
  return pathsAndTimes;
}

} // namespace AN
