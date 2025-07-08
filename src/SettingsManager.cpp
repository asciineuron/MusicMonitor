#include "SettingsManager.hpp"
#include "FoldersManager.hpp"
#include <filesystem>
#include <fstream>
#include <stdexcept>

namespace AN {
namespace fs = std::filesystem;

SettingsManager::SettingsManager(fs::path settingsFile) {
  if (!fs::is_regular_file(settingsFile)) {
    throw std::invalid_argument("Settings file does not exist");
  }

  std::ifstream file(settingsFile);
  m_json = Json::parse(file);
}

std::vector<FileSettings> SettingsManager::getFileSettings() {
  std::vector<FileSettings> allFileSettings;
  for (auto &filetypesetting : m_json["filetype_settings"]) {
    FileSettings settings;
    settings.extension = filetypesetting["extension"].template get<std::string>();
    settings.cmd = filetypesetting["cmd"].template get<std::string>();
    settings.keep = filetypesetting["keep"].template get<bool>();

    allFileSettings.push_back(settings);
  }
  return allFileSettings;
}

}; // namespace AN

// // struct FileSettings {
//   std::string extension;
//   fs::path cmd{"/bin/echo"};
//   bool keep{true};
// };
