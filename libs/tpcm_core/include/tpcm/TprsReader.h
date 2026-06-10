#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace tpcm {

struct TprsMeta {
    std::string rawJson;
    std::string title;
    std::string author;
    std::vector<int> drumChannels;
    int masterVol = 127;
    bool loopEnabled = true;
    int loopStart = 0;
    int loopEnd = 0;
    std::string channelOverridesJson;
    bool noEnemyMusic = false;
};

struct TprsPackage {
    std::vector<std::uint8_t> midiBytes;
    std::vector<int> categories;
    TprsMeta meta;
};

class TprsReader {
public:
    explicit TprsReader(std::filesystem::path path);

    TprsPackage read() const;
    std::vector<std::string> validate() const;

private:
    std::filesystem::path m_path;
};

bool isKnownTprsCategory(int category) noexcept;
bool isCompatibleWithCategory(const std::vector<int>& categories, int bmsCategory);
void updateTprsMeta(const std::filesystem::path& path, const std::string& metaJson);
void createTprs(const std::filesystem::path& path,
                const std::vector<std::uint8_t>& midiBytes,
                const std::string& midiName,
                const std::string& metaJson,
                const std::vector<int>& categories);

}  // namespace tpcm
