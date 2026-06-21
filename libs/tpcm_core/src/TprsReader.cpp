#include "tpcm/TprsReader.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <utility>

#ifdef TPCM_HAS_ZLIB
#include <zlib.h>
#endif

namespace tpcm {
namespace {

constexpr std::uint32_t kEocdSignature = 0x06054B50;
constexpr std::uint32_t kCentralDirectorySignature = 0x02014B50;
constexpr std::uint32_t kLocalFileSignature = 0x04034B50;
constexpr std::uint16_t kCompressionStored = 0;
constexpr std::uint16_t kCompressionDeflated = 8;

struct ZipEntry {
    std::string name;
    std::uint16_t compressionMethod = 0;
    std::uint32_t compressedSize = 0;
    std::uint32_t uncompressedSize = 0;
    std::uint32_t localHeaderOffset = 0;
};

std::uint16_t readU16(const std::vector<std::uint8_t>& data, std::size_t offset) {
    if (offset + 2 > data.size()) {
        throw std::runtime_error("Truncated ZIP data");
    }
    return static_cast<std::uint16_t>(data[offset] | (data[offset + 1] << 8));
}

std::uint32_t readU32(const std::vector<std::uint8_t>& data, std::size_t offset) {
    if (offset + 4 > data.size()) {
        throw std::runtime_error("Truncated ZIP data");
    }
    return static_cast<std::uint32_t>(data[offset] | (data[offset + 1] << 8) | (data[offset + 2] << 16)
                                      | (data[offset + 3] << 24));
}

std::vector<std::uint8_t> readFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("Could not open TPRS archive");
    }
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

std::size_t findEocd(const std::vector<std::uint8_t>& data) {
    const std::size_t minSize = 22;
    if (data.size() < minSize) {
        throw std::runtime_error("File is too small to be a ZIP archive");
    }

    const std::size_t maxComment = std::min<std::size_t>(data.size() - minSize, 0xFFFF);
    for (std::size_t back = 0; back <= maxComment; ++back) {
        const std::size_t offset = data.size() - minSize - back;
        if (readU32(data, offset) == kEocdSignature) {
            return offset;
        }
    }
    throw std::runtime_error("Missing ZIP end-of-central-directory record");
}

std::vector<ZipEntry> readCentralDirectory(const std::vector<std::uint8_t>& data) {
    const std::size_t eocd = findEocd(data);
    const std::uint16_t entryCount = readU16(data, eocd + 10);
    const std::uint32_t centralOffset = readU32(data, eocd + 16);

    std::vector<ZipEntry> entries;
    entries.reserve(entryCount);

    std::size_t offset = centralOffset;
    for (std::uint16_t i = 0; i < entryCount; ++i) {
        if (readU32(data, offset) != kCentralDirectorySignature) {
            throw std::runtime_error("Invalid ZIP central-directory entry");
        }

        const std::uint16_t nameLen = readU16(data, offset + 28);
        const std::uint16_t extraLen = readU16(data, offset + 30);
        const std::uint16_t commentLen = readU16(data, offset + 32);
        const std::size_t nameOffset = offset + 46;
        if (nameOffset + nameLen > data.size()) {
            throw std::runtime_error("Truncated ZIP central-directory name");
        }

        ZipEntry entry;
        entry.compressionMethod = readU16(data, offset + 10);
        entry.compressedSize = readU32(data, offset + 20);
        entry.uncompressedSize = readU32(data, offset + 24);
        entry.localHeaderOffset = readU32(data, offset + 42);
        entry.name = std::string(reinterpret_cast<const char*>(data.data() + nameOffset), nameLen);
        entries.push_back(entry);

        offset = nameOffset + nameLen + extraLen + commentLen;
    }

    return entries;
}

std::string lowerAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

bool isMidiName(const std::string& name) {
    const std::string lower = lowerAscii(name);
    return lower.size() >= 4
           && (lower.compare(lower.size() - 4, 4, ".mid") == 0
               || (lower.size() >= 5 && lower.compare(lower.size() - 5, 5, ".midi") == 0));
}

const ZipEntry* findEntry(const std::vector<ZipEntry>& entries, const std::string& name) {
    for (const ZipEntry& entry : entries) {
        if (entry.name == name) {
            return &entry;
        }
    }
    return nullptr;
}

const ZipEntry* findMidiEntry(const std::vector<ZipEntry>& entries) {
    for (const ZipEntry& entry : entries) {
        if (isMidiName(entry.name)) {
            return &entry;
        }
    }
    return nullptr;
}

std::vector<std::uint8_t> inflateRawDeflate(const std::uint8_t* input,
                                            std::size_t inputSize,
                                            std::size_t outputSize) {
#ifdef TPCM_HAS_ZLIB
    std::vector<std::uint8_t> output(outputSize);
    z_stream stream{};
    stream.next_in = const_cast<Bytef*>(reinterpret_cast<const Bytef*>(input));
    stream.avail_in = static_cast<uInt>(inputSize);
    stream.next_out = reinterpret_cast<Bytef*>(output.data());
    stream.avail_out = static_cast<uInt>(output.size());

    if (inflateInit2(&stream, -MAX_WBITS) != Z_OK) {
        throw std::runtime_error("Could not initialize zlib inflate");
    }
    const int result = inflate(&stream, Z_FINISH);
    inflateEnd(&stream);

    if (result != Z_STREAM_END || stream.total_out != outputSize) {
        throw std::runtime_error("Could not inflate ZIP entry");
    }
    return output;
#else
    (void)input;
    (void)inputSize;
    (void)outputSize;
    throw std::runtime_error("Deflated ZIP entries require zlib support");
#endif
}

std::vector<std::uint8_t> extractEntry(const std::vector<std::uint8_t>& data, const ZipEntry& entry) {
    const std::size_t local = entry.localHeaderOffset;
    if (readU32(data, local) != kLocalFileSignature) {
        throw std::runtime_error("Invalid ZIP local-file header");
    }

    const std::uint16_t nameLen = readU16(data, local + 26);
    const std::uint16_t extraLen = readU16(data, local + 28);
    const std::size_t dataOffset = local + 30 + nameLen + extraLen;
    if (dataOffset + entry.compressedSize > data.size()) {
        throw std::runtime_error("Truncated ZIP entry data");
    }

    const std::uint8_t* payload = data.data() + dataOffset;
    if (entry.compressionMethod == kCompressionStored) {
        if (entry.compressedSize != entry.uncompressedSize) {
            throw std::runtime_error("Invalid stored ZIP entry sizes");
        }
        return {payload, payload + entry.uncompressedSize};
    }
    if (entry.compressionMethod == kCompressionDeflated) {
        return inflateRawDeflate(payload, entry.compressedSize, entry.uncompressedSize);
    }

    throw std::runtime_error("Unsupported ZIP compression method");
}

std::string bytesToString(const std::vector<std::uint8_t>& bytes) {
    return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

std::vector<int> parseCategories(const std::string& text) {
    std::vector<int> categories;
    std::istringstream lines(text);
    std::string line;
    while (std::getline(lines, line)) {
        const std::size_t first = line.find_first_not_of(" \t\r\n");
        if (first == std::string::npos || line[first] == '#') {
            continue;
        }
        try {
            categories.push_back(std::stoi(line.substr(first)));
        } catch (...) {
        }
    }
    return categories;
}

void appendU16(std::vector<std::uint8_t>& data, std::uint16_t value) {
    data.push_back(static_cast<std::uint8_t>(value & 0xFF));
    data.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
}

void appendU32(std::vector<std::uint8_t>& data, std::uint32_t value) {
    appendU16(data, static_cast<std::uint16_t>(value & 0xFFFF));
    appendU16(data, static_cast<std::uint16_t>((value >> 16) & 0xFFFF));
}

std::uint32_t crc32Of(const std::vector<std::uint8_t>& data) {
    std::uint32_t crc = 0xFFFFFFFFu;
    for (const std::uint8_t byte : data) {
        crc ^= byte;
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

void writeStoredZip(const std::filesystem::path& path,
                    const std::vector<std::pair<std::string, std::vector<std::uint8_t>>>& files) {
    std::vector<std::uint8_t> zip;
    std::vector<std::uint32_t> offsets;
    std::vector<std::uint32_t> crcs;
    for (const auto& file : files) {
        offsets.push_back(static_cast<std::uint32_t>(zip.size()));
        crcs.push_back(crc32Of(file.second));
        appendU32(zip, kLocalFileSignature);
        appendU16(zip, 20); appendU16(zip, 0); appendU16(zip, kCompressionStored);
        appendU16(zip, 0); appendU16(zip, 0);
        appendU32(zip, crcs.back());
        appendU32(zip, static_cast<std::uint32_t>(file.second.size()));
        appendU32(zip, static_cast<std::uint32_t>(file.second.size()));
        appendU16(zip, static_cast<std::uint16_t>(file.first.size())); appendU16(zip, 0);
        zip.insert(zip.end(), file.first.begin(), file.first.end());
        zip.insert(zip.end(), file.second.begin(), file.second.end());
    }

    const std::uint32_t centralOffset = static_cast<std::uint32_t>(zip.size());
    for (std::size_t i = 0; i < files.size(); ++i) {
        appendU32(zip, kCentralDirectorySignature);
        appendU16(zip, 20); appendU16(zip, 20); appendU16(zip, 0);
        appendU16(zip, kCompressionStored); appendU16(zip, 0); appendU16(zip, 0);
        appendU32(zip, crcs[i]);
        appendU32(zip, static_cast<std::uint32_t>(files[i].second.size()));
        appendU32(zip, static_cast<std::uint32_t>(files[i].second.size()));
        appendU16(zip, static_cast<std::uint16_t>(files[i].first.size()));
        appendU16(zip, 0); appendU16(zip, 0); appendU16(zip, 0); appendU16(zip, 0);
        appendU32(zip, 0); appendU32(zip, offsets[i]);
        zip.insert(zip.end(), files[i].first.begin(), files[i].first.end());
    }
    const std::uint32_t centralSize = static_cast<std::uint32_t>(zip.size()) - centralOffset;
    appendU32(zip, kEocdSignature);
    appendU16(zip, 0); appendU16(zip, 0);
    appendU16(zip, static_cast<std::uint16_t>(files.size()));
    appendU16(zip, static_cast<std::uint16_t>(files.size()));
    appendU32(zip, centralSize); appendU32(zip, centralOffset); appendU16(zip, 0);

    std::ofstream output(path, std::ios::binary);
    if (!output) throw std::runtime_error("Could not write TPRS archive");
    output.write(reinterpret_cast<const char*>(zip.data()), static_cast<std::streamsize>(zip.size()));
    if (!output) throw std::runtime_error("Could not write TPRS archive");
}

std::string parseJsonStringField(const std::string& json, const std::string& key) {
    const std::string quotedKey = "\"" + key + "\"";
    std::size_t pos = json.find(quotedKey);
    if (pos == std::string::npos) {
        return {};
    }
    pos = json.find(':', pos + quotedKey.size());
    if (pos == std::string::npos) {
        return {};
    }
    pos = json.find('"', pos + 1);
    if (pos == std::string::npos) {
        return {};
    }

    std::string value;
    bool escaped = false;
    for (std::size_t i = pos + 1; i < json.size(); ++i) {
        const char c = json[i];
        if (escaped) {
            value.push_back(c);
            escaped = false;
        } else if (c == '\\') {
            escaped = true;
        } else if (c == '"') {
            return value;
        } else {
            value.push_back(c);
        }
    }
    return {};
}

int parseJsonIntField(const std::string& json, const std::string& key, int defaultValue) {
    const std::string quotedKey = "\"" + key + "\"";
    std::size_t pos = json.find(quotedKey);
    if (pos == std::string::npos) {
        return defaultValue;
    }
    pos = json.find(':', pos + quotedKey.size());
    if (pos == std::string::npos) {
        return defaultValue;
    }

    const std::size_t first = json.find_first_of("-0123456789", pos + 1);
    if (first == std::string::npos) {
        return defaultValue;
    }
    try {
        return std::stoi(json.substr(first));
    } catch (...) {
        return defaultValue;
    }
}

bool parseJsonBoolField(const std::string& json, const std::string& key, bool defaultValue) {
    const std::string quotedKey = "\"" + key + "\"";
    std::size_t pos = json.find(quotedKey);
    if (pos == std::string::npos) return defaultValue;
    pos = json.find(':', pos + quotedKey.size());
    if (pos == std::string::npos) return defaultValue;
    const std::size_t first = json.find_first_not_of(" \t\r\n", pos + 1);
    if (first == std::string::npos) return defaultValue;
    if (json.compare(first, 4, "true") == 0) return true;
    if (json.compare(first, 5, "false") == 0) return false;
    return parseJsonIntField(json, key, defaultValue ? 1 : 0) != 0;
}

std::vector<int> parseJsonIntArrayField(const std::string& json, const std::string& key) {
    std::vector<int> values;
    const std::string quotedKey = "\"" + key + "\"";
    std::size_t pos = json.find(quotedKey);
    if (pos == std::string::npos) {
        return values;
    }
    pos = json.find('[', pos + quotedKey.size());
    if (pos == std::string::npos) {
        return values;
    }
    const std::size_t end = json.find(']', pos + 1);
    if (end == std::string::npos) {
        return values;
    }

    std::istringstream stream(json.substr(pos + 1, end - pos - 1));
    std::string token;
    while (std::getline(stream, token, ',')) {
        try {
            values.push_back(std::stoi(token));
        } catch (...) {
        }
    }
    return values;
}

std::string parseJsonObjectField(const std::string& json, const std::string& key) {
    const std::string quotedKey = "\"" + key + "\"";
    std::size_t pos = json.find(quotedKey);
    if (pos == std::string::npos) {
        return {};
    }
    pos = json.find('{', pos + quotedKey.size());
    if (pos == std::string::npos) {
        return {};
    }

    int depth = 0;
    bool inString = false;
    bool escaped = false;
    for (std::size_t i = pos; i < json.size(); ++i) {
        const char c = json[i];
        if (escaped) {
            escaped = false;
            continue;
        }
        if (c == '\\') {
            escaped = true;
            continue;
        }
        if (c == '"') {
            inString = !inString;
            continue;
        }
        if (inString) {
            continue;
        }
        if (c == '{') {
            ++depth;
        } else if (c == '}') {
            --depth;
            if (depth == 0) {
                return json.substr(pos, i - pos + 1);
            }
        }
    }
    return {};
}

TprsMeta parseMeta(const std::string& json) {
    TprsMeta meta;
    meta.rawJson = json;
    meta.title = parseJsonStringField(json, "title");
    meta.author = parseJsonStringField(json, "author");
    meta.drumChannels = parseJsonIntArrayField(json, "drum_channels");
    meta.masterVol = parseJsonIntField(json, "master_vol", 127);
    meta.loopEnabled = parseJsonBoolField(json, "loop_enabled", true);
    meta.loopStart = parseJsonIntField(json, "loop_start", 0);
    meta.loopEnd = parseJsonIntField(json, "loop_end", 0);
    meta.channelOverridesJson = parseJsonObjectField(json, "channel_overrides");
    meta.noEnemyMusic = parseJsonBoolField(json, "no_enemy_music", false);
    return meta;
}

bool containsCategory(const std::vector<int>& categories, int category) {
    return std::find(categories.begin(), categories.end(), category) != categories.end();
}

}  // namespace

TprsReader::TprsReader(std::filesystem::path path) : m_path(std::move(path)) {}

TprsPackage TprsReader::read() const {
    const std::vector<std::uint8_t> archive = readFile(m_path);
    const std::vector<ZipEntry> entries = readCentralDirectory(archive);

    const ZipEntry* midi = findMidiEntry(entries);
    if (midi == nullptr) {
        throw std::runtime_error("TPRS archive has no .mid/.midi file");
    }

    const ZipEntry* categories = findEntry(entries, "categories.txt");
    if (categories == nullptr) {
        throw std::runtime_error("TPRS archive is missing categories.txt");
    }

    TprsPackage package;
    package.midiBytes = extractEntry(archive, *midi);
    package.categories = parseCategories(bytesToString(extractEntry(archive, *categories)));

    if (const ZipEntry* meta = findEntry(entries, "meta.json")) {
        package.meta = parseMeta(bytesToString(extractEntry(archive, *meta)));
    }

    if (package.meta.title.empty()) {
        package.meta.title = m_path.stem().string();
    }

    return package;
}

std::vector<std::string> TprsReader::validate() const {
    std::vector<std::string> problems;
    try {
        const std::vector<std::uint8_t> archive = readFile(m_path);
        const std::vector<ZipEntry> entries = readCentralDirectory(archive);

        if (findMidiEntry(entries) == nullptr) {
            problems.push_back("No .mid/.midi file found in archive");
        }

        const ZipEntry* categoriesEntry = findEntry(entries, "categories.txt");
        if (categoriesEntry == nullptr) {
            problems.push_back("Missing categories.txt");
        } else {
            const std::vector<int> categories =
                parseCategories(bytesToString(extractEntry(archive, *categoriesEntry)));
            if (categories.empty()) {
                problems.push_back("categories.txt is empty; no categories declared");
            }
            for (const int category : categories) {
                if (!isKnownTprsCategory(category)) {
                    problems.push_back("Unknown category " + std::to_string(category) + " in categories.txt");
                }
            }
        }
    } catch (const std::exception& ex) {
        problems.push_back(ex.what());
    }
    return problems;
}

bool isKnownTprsCategory(int category) noexcept {
    switch (category) {
        case 0:
        case 1:
        case 2:
        case 3:
        case 5:
        case 6:
        case 7:
        case 8:
        case 9:
        case 10:
        case 11:
        case 12:
            return true;
        default:
            return false;
    }
}

bool isCompatibleWithCategory(const std::vector<int>& categories, int bmsCategory) {
    return containsCategory(categories, 0) || containsCategory(categories, bmsCategory);
}

void updateTprsMeta(const std::filesystem::path& path, const std::string& metaJson) {
    const std::vector<std::uint8_t> archive = readFile(path);
    const std::vector<ZipEntry> entries = readCentralDirectory(archive);
    std::vector<std::pair<std::string, std::vector<std::uint8_t>>> files;
    bool replaced = false;
    for (const ZipEntry& entry : entries) {
        if (!entry.name.empty() && entry.name.back() == '/') continue;
        if (entry.name == "meta.json") {
            files.push_back({entry.name, {metaJson.begin(), metaJson.end()}});
            replaced = true;
        } else {
            files.push_back({entry.name, extractEntry(archive, entry)});
        }
    }
    if (!replaced) files.push_back({"meta.json", {metaJson.begin(), metaJson.end()}});

    const std::filesystem::path temporary = path.string() + ".tmp";
    writeStoredZip(temporary, files);
    std::filesystem::copy_file(temporary, path, std::filesystem::copy_options::overwrite_existing);
    std::filesystem::remove(temporary);
}

void createTprs(const std::filesystem::path& path,
                const std::vector<std::uint8_t>& midiBytes,
                const std::string& midiName,
                const std::string& metaJson,
                const std::vector<int>& categories) {
    std::string categoriesTxt;
    for (const int cat : categories) {
        categoriesTxt += std::to_string(cat) + "\n";
    }
    if (categoriesTxt.empty()) {
        categoriesTxt = "0\n";
    }
    writeStoredZip(path, {
        {midiName, midiBytes},
        {"meta.json", {metaJson.begin(), metaJson.end()}},
        {"categories.txt", {categoriesTxt.begin(), categoriesTxt.end()}},
    });
}

}  // namespace tpcm
