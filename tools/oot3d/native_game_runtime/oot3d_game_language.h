#pragma once

#include <cstdint>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace Oot3dNativeGame {
struct GameLanguage {
  std::string Code;
  std::string Label;
  uint8_t SystemId;
};

// Reads native menu resources and populated QM message slots, not ROM
// filenames.
std::vector<GameLanguage>
DetectGameLanguages(const std::filesystem::path &romfs, uint64_t offset = 0,
                    uint64_t size = 0);
nlohmann::json GameLanguageDocument(const std::vector<GameLanguage> &languages,
                                    const std::string &selected = "en");

// The language folders of the UI texture pack for a 3DS system language,
// most specific first: the native menu folders (rom:/menu/<folder>/, Europe
// before North America) and the pack's per-language family folder.
std::vector<std::string> UiTexturePackLanguageFolders(uint8_t systemLanguage);

class GameLanguageSettings {
public:
  GameLanguageSettings(std::filesystem::path path,
                       std::vector<GameLanguage> available);
  bool Select(const std::string &code);
  uint8_t SystemId() const;
  const std::vector<GameLanguage> &Available() const { return mAvailable; }
  const std::string &Selected() const { return mSelected; }
  const std::string &BootLanguage() const { return mBootLanguage; }
  const std::string &Error() const { return mError; }

private:
  std::filesystem::path mPath;
  std::vector<GameLanguage> mAvailable;
  std::string mSelected, mBootLanguage, mError;
};
} // namespace Oot3dNativeGame
