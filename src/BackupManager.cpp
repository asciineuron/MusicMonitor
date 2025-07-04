#include "BackupManager.hpp"
#include "FoldersManager.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>

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

FSEventStreamEventId JsonManager::getLastObservedEventId() {
  // FSEventStreamEventId lastEvent = m_json.contains("lasteventid") ?
  // static_cast<FSEventStreamEventId>(m_json["lasteventid"]) :
  // kFSEventStreamEventIdSinceNow;
  FSEventStreamEventId lastEvent;
  if (m_json.contains("lasteventid")) {
    std::cout << "restoring latest event id: " << lastEvent;
    lastEvent = static_cast<FSEventStreamEventId>(m_json["lasteventid"]);
  } else {
    std::cout << "starting timestamp now\n";
    lastEvent = kFSEventStreamEventIdSinceNow;
  }
  return lastEvent;
}

JsonManager::JsonManager(fs::path backupFile) {
  m_backupFile = backupFile;
  if (fs::exists(m_backupFile)) {
    std::ifstream file(backupFile.string());
    m_json = Json::parse(file);
  }
}

void JsonManager::updateBackup() {
  std::ofstream backupFileOut(m_backupFile);
  backupFileOut << m_json.dump(4);
}

void JsonManager::getFolderScannerUpdate(FolderScanner &scanner) {
  auto filesAndTimes = scanner.getFilesAndTimes();
  fs::path root = scanner.getRoot();
  Json entry;
  entry["folder_root"] = root;
  for (std::pair<fs::path, time_t> &fileAndTime : filesAndTimes) {
    Json fileEntry;
    fileEntry["path"] = fileAndTime.first.string();
    fileEntry["time"] = fileAndTime.second;
    entry["paths_and_times"].push_back(fileEntry);
  }
  m_json["folder_scan_list"].push_back(entry);
}

void JsonManager::getFolderManagerUpdate(FoldersManager &manager) {
  m_json["last_event_id"] = manager.getLatestEventId();
}

} // namespace AN
