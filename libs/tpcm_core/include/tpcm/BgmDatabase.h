#pragma once

#include <cstddef>
#include <cstdint>

namespace tpcm {

enum class BmsCategory : std::uint8_t {
    Any = 0,
    Overworld = 1,
    Village = 2,
    Nature = 3,
    Twilight = 5,
    Dungeon = 6,
    Interior = 7,
    Shop = 8,
    Minigame = 9,
    Event = 10,
    Menu = 11,
    Boss = 12,
};

enum class BgmTrigger : std::uint8_t {
    Scene,
    Main,
    Sub,
    Fanfare,
};

struct BgmEntry {
    const char* bmsName;
    const char* displayName;
    const char* internalName;
    std::uint32_t bgmId;
    std::uint8_t bgmWave1;
    std::uint8_t bgmWave2;
    std::uint8_t pinnedBank;
    BmsCategory category;
    BgmTrigger trigger;
    bool sceneBgm;
    bool dungeonBgm;
    bool minibossBgm;
    bool bossBgm;
    bool minigameBgm;
    bool eventBgm;
    bool cutsceneBgm;
    bool isFanfare;
};

constexpr std::uint32_t kBgmIdPrefix = 0x01000000;

constexpr std::uint32_t MakeBgmId(std::uint8_t lowByte) noexcept {
    return kBgmIdPrefix | lowByte;
}

extern const BgmEntry BGM_TABLE[];
extern const std::size_t BGM_TABLE_SIZE;

const BgmEntry* FindBgmById(std::uint32_t bgmId) noexcept;

}  // namespace tpcm
