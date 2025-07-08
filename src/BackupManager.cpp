#include "BackupManager.hpp"
#include "FoldersManager.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>

namespace AN {

bool JsonManager::isMonitoredRoot(fs::path path) {
  for (const Json folderScanner : m_jsonIn["folder_scan_list"]) {
    if (folderScanner["folder_root"] == path.string()) {
      return true;
    }
  }
  return false;
}

std::vector<std::pair<fs::path, time_t>>
JsonManager::getRootMonitoredFiles(fs::path path) {
  std::vector<std::pair<fs::path, time_t>> pathsAndTimes;

  for (const Json &folderScanner : m_jsonIn["folder_scan_list"]) {
    if (folderScanner["folder_root"] != path.string())
      continue;

    for (const Json &jPathsAndTimes : folderScanner["paths_and_times"]) {
      fs::path path(jPathsAndTimes["path"]);
      time_t time(jPathsAndTimes["time"].template get<int>());

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
  if (m_jsonIn.contains("lasteventid")) {
    lastEvent = static_cast<FSEventStreamEventId>(m_jsonIn["lasteventid"].template get<FSEventStreamEventId>());
    std::cout << "restoring latest event id: " << lastEvent;
  } else {
    lastEvent = kFSEventStreamEventIdSinceNow;
    std::cout << "starting timestamp now: " << lastEvent << "\n";
  }
  return lastEvent;
}

JsonManager::JsonManager(fs::path backupFile) {
  m_backupFile = backupFile;
  if (fs::exists(m_backupFile)) {
    std::ifstream file(backupFile.string());
    m_jsonIn = Json::parse(file);
  }
}

void JsonManager::updateBackup() {
  // remove existing backup
  std::ofstream backupFileOut(m_backupFile, std::ios::trunc);
  // tab=2spaces
  backupFileOut << m_jsonOut.dump(2);
}

void JsonManager::getFolderScannerUpdate(FolderScanner &scanner) {
  auto filesAndTimes = scanner.getFilesAndTimes();
  fs::path root = scanner.getRoot();
  Json entry;
  entry["folder_root"] = root;
  for (const std::pair<fs::path, time_t> &fileAndTime : filesAndTimes) {
    Json fileEntry;
    fileEntry["path"] = fileAndTime.first.string();
    fileEntry["time"] = fileAndTime.second;
    entry["paths_and_times"].push_back(fileEntry);
  }
  if (entry.contains("paths_and_times")) {
    m_jsonOut["folder_scan_list"].push_back(entry);
  }
}

void JsonManager::getFolderManagerUpdate(FoldersManager &manager) {
  m_jsonOut["last_event_id"] = manager.getLatestEventId();
}

} // namespace AN
