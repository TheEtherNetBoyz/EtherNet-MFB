#include "tpcm/InstrumentMap.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <stdexcept>

namespace tpcm {
namespace {

// ============================================================
// Minimal JSON parser for the instrument map format.
// Handles exactly: {"schema":N,"zones":[{"bank":N,...},...]}.
// All values are non-negative integers.
// ============================================================

size_t skipWs(const std::string& s, size_t pos) {
    while (pos < s.size() && std::isspace(static_cast<unsigned char>(s[pos]))) ++pos;
    return pos;
}

// Find the closing bracket/brace that matches the one at s[open].
// Skips over string literals so quoted brackets don't confuse the count.
size_t findClose(const std::string& s, size_t open, char openCh, char closeCh) {
    int depth = 0;
    bool inStr = false;
    for (size_t i = open; i < s.size(); ++i) {
        if (inStr) {
            if (s[i] == '\\') { ++i; continue; }
            if (s[i] == '"')  { inStr = false; }
        } else {
            if (s[i] == '"')    { inStr = true; continue; }
            if (s[i] == openCh) { ++depth; }
            else if (s[i] == closeCh) {
                if (--depth == 0) return i;
            }
        }
    }
    return std::string::npos;
}

// Extract a named integer field within s[start..end).
// Searches for "key": N and returns false if not found.
bool extractInt(const std::string& s, size_t start, size_t end,
                const char* key, int& out)
{
    const std::string pattern = std::string("\"") + key + "\"";
    size_t pos = s.find(pattern, start);
    if (pos == std::string::npos || pos >= end) return false;
    pos = skipWs(s, pos + pattern.size());
    if (pos >= end || s[pos] != ':') return false;
    pos = skipWs(s, pos + 1);
    if (pos >= end) return false;
    if (!std::isdigit(static_cast<unsigned char>(s[pos]))) return false;
    int val = 0;
    while (pos < end && std::isdigit(static_cast<unsigned char>(s[pos])))
        val = val * 10 + (s[pos++] - '0');
    out = val;
    return true;
}

}  // namespace

InstrumentMap InstrumentMap::loadFromJson(const std::string& json) {
    InstrumentMap map;

    // Locate "zones" array
    const std::string zonesKey = "\"zones\"";
    const size_t zonesPos = json.find(zonesKey);
    if (zonesPos == std::string::npos)
        throw std::runtime_error("InstrumentMap: no 'zones' key");

    const size_t bracketOpen = json.find('[', zonesPos + zonesKey.size());
    if (bracketOpen == std::string::npos)
        throw std::runtime_error("InstrumentMap: no '[' after 'zones'");

    const size_t bracketClose = findClose(json, bracketOpen, '[', ']');
    if (bracketClose == std::string::npos)
        throw std::runtime_error("InstrumentMap: unclosed '[' in 'zones'");

    // Iterate objects within the array
    size_t pos = bracketOpen + 1;
    while (pos < bracketClose) {
        pos = skipWs(json, pos);
        if (pos >= bracketClose) break;
        if (json[pos] != '{') { ++pos; continue; }

        const size_t objClose = findClose(json, pos, '{', '}');
        if (objClose == std::string::npos) break;

        int bank, program, loKey, hiKey, waveId;
        if (extractInt(json, pos, objClose, "bank",    bank)    &&
            extractInt(json, pos, objClose, "program", program) &&
            extractInt(json, pos, objClose, "loKey",   loKey)   &&
            extractInt(json, pos, objClose, "hiKey",   hiKey)   &&
            extractInt(json, pos, objClose, "waveId",  waveId))
        {
            InstrumentZone zone;
            zone.bank    = static_cast<uint8_t>(bank);
            zone.program = static_cast<uint8_t>(program);
            zone.loKey   = static_cast<uint8_t>(loKey);
            zone.hiKey   = static_cast<uint8_t>(hiKey);
            zone.waveId  = static_cast<uint16_t>(waveId);
            map.m_zones.push_back(zone);
        }

        pos = objClose + 1;
    }

    map.buildIndex();
    return map;
}

InstrumentMap InstrumentMap::loadFromFile(const std::string& path) {
    std::ifstream f(path);
    if (!f) throw std::runtime_error("Cannot open instrument map: " + path);
    return loadFromJson({std::istreambuf_iterator<char>(f), {}});
}

void InstrumentMap::buildIndex() {
    // Sort so zones for the same (bank, program) are contiguous.
    std::stable_sort(m_zones.begin(), m_zones.end(), [](const InstrumentZone& a, const InstrumentZone& b) {
        if (a.bank != b.bank) return a.bank < b.bank;
        return a.program < b.program;
    });

    m_index.clear();
    for (size_t i = 0; i < m_zones.size(); ) {
        const uint16_t key = (uint16_t(m_zones[i].bank) << 8) | m_zones[i].program;
        size_t j = i;
        while (j < m_zones.size() &&
               m_zones[j].bank    == m_zones[i].bank &&
               m_zones[j].program == m_zones[i].program) ++j;
        m_index[key] = {i, j};
        i = j;
    }
}

uint16_t InstrumentMap::findWaveId(uint8_t bank, uint8_t program, uint8_t midiKey) const {
    const uint16_t key = (uint16_t(bank) << 8) | program;
    const auto it = m_index.find(key);
    if (it == m_index.end()) return 0xFFFF;
    const size_t first = it->second.first;
    const size_t last  = it->second.second;
    for (size_t i = first; i < last; ++i) {
        const InstrumentZone& z = m_zones[i];
        if (midiKey >= z.loKey && midiKey <= z.hiKey)
            return z.waveId;
    }
    return 0xFFFF;
}

std::vector<InstrumentZone> InstrumentMap::zonesFor(uint8_t bank, uint8_t program) const {
    const uint16_t key = (uint16_t(bank) << 8) | program;
    const auto it = m_index.find(key);
    if (it == m_index.end()) return {};
    return {m_zones.begin() + static_cast<ptrdiff_t>(it->second.first),
            m_zones.begin() + static_cast<ptrdiff_t>(it->second.second)};
}

}  // namespace tpcm
