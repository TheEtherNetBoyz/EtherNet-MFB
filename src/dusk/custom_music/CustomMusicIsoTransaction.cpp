#include "dusk/custom_music/CustomMusicIsoTransaction.h"

#include <tpcm/IsoFileSystem.h>

#include "nlohmann/json.hpp"

#include <array>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace dusk::custom_music {
namespace {

constexpr std::array<std::uint32_t, 64> ShaConstants = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4,
    0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe,
    0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f,
    0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
    0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc,
    0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116,
    0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
    0xc67178f2,
};

std::uint32_t rotateRight(std::uint32_t value, unsigned bits) {
    return (value >> bits) | (value << (32 - bits));
}

std::string sha256(const fs::path& path, const std::atomic<bool>& cancel,
                   const TransactionProgress& progress, const std::string& stage,
                   float progressStart, float progressEnd, const fs::path& copyDestination = {}) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("Cannot hash " + path.string());
    std::ofstream copy;
    if (!copyDestination.empty()) {
        copy.open(copyDestination, std::ios::binary | std::ios::trunc);
        if (!copy) throw std::runtime_error("Cannot create " + copyDestination.string());
    }
    const std::uint64_t totalSize = fs::file_size(path);
    std::array<std::uint32_t, 8> hash = {
        0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
        0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19,
    };
    const auto processBlock = [&](const std::uint8_t* bytes) {
        std::array<std::uint32_t, 64> words{};
        for (std::size_t i = 0; i < 16; ++i) {
            const std::size_t p = i * 4;
            words[i] = (std::uint32_t(bytes[p]) << 24) | (std::uint32_t(bytes[p + 1]) << 16)
                     | (std::uint32_t(bytes[p + 2]) << 8) | std::uint32_t(bytes[p + 3]);
        }
        for (std::size_t i = 16; i < words.size(); ++i) {
            const auto s0 = rotateRight(words[i - 15], 7) ^ rotateRight(words[i - 15], 18) ^ (words[i - 15] >> 3);
            const auto s1 = rotateRight(words[i - 2], 17) ^ rotateRight(words[i - 2], 19) ^ (words[i - 2] >> 10);
            words[i] = words[i - 16] + s0 + words[i - 7] + s1;
        }
        auto a = hash[0], b = hash[1], c = hash[2], d = hash[3];
        auto e = hash[4], f = hash[5], g = hash[6], h = hash[7];
        for (std::size_t i = 0; i < words.size(); ++i) {
            const auto s1 = rotateRight(e, 6) ^ rotateRight(e, 11) ^ rotateRight(e, 25);
            const auto choice = (e & f) ^ (~e & g);
            const auto temp1 = h + s1 + choice + ShaConstants[i] + words[i];
            const auto s0 = rotateRight(a, 2) ^ rotateRight(a, 13) ^ rotateRight(a, 22);
            const auto majority = (a & b) ^ (a & c) ^ (b & c);
            const auto temp2 = s0 + majority;
            h = g; g = f; f = e; e = d + temp1; d = c; c = b; b = a; a = temp1 + temp2;
        }
        hash[0] += a; hash[1] += b; hash[2] += c; hash[3] += d;
        hash[4] += e; hash[5] += f; hash[6] += g; hash[7] += h;
    };

    std::array<std::uint8_t, 64> block{};
    std::vector<std::uint8_t> readBuffer(4 * 1024 * 1024);
    std::uint64_t byteLength = 0;
    std::size_t finalBlockSize = 0;
    while (input) {
        if (cancel.load()) throw std::runtime_error("Operation cancelled.");
        input.read(reinterpret_cast<char*>(readBuffer.data()), readBuffer.size());
        const std::streamsize count = input.gcount();
        if (copy && count > 0) {
            copy.write(reinterpret_cast<const char*>(readBuffer.data()), count);
            if (!copy) throw std::runtime_error("Failed writing " + copyDestination.string());
        }
        byteLength += static_cast<std::uint64_t>(count);
        if (progress && totalSize > 0) {
            const float ratio = static_cast<float>(byteLength) / static_cast<float>(totalSize);
            progress(stage, progressStart + (progressEnd - progressStart) * ratio);
        }
        const std::size_t bytesRead = static_cast<std::size_t>(count);
        std::size_t offset = 0;
        while (offset + block.size() <= bytesRead) {
            processBlock(readBuffer.data() + offset);
            offset += block.size();
        }
        finalBlockSize = bytesRead - offset;
        if (finalBlockSize > 0) {
            std::copy_n(readBuffer.data() + offset, finalBlockSize, block.data());
        }
    }
    std::size_t used = finalBlockSize;
    block[used++] = 0x80;
    if (used > 56) {
        std::fill(block.begin() + static_cast<std::ptrdiff_t>(used), block.end(), 0);
        processBlock(block.data());
        block.fill(0);
        used = 0;
    }
    std::fill(block.begin() + static_cast<std::ptrdiff_t>(used), block.begin() + 56, 0);
    const std::uint64_t bitLength = byteLength * 8;
    for (int shift = 56, index = 56; shift >= 0; shift -= 8, ++index) {
        block[static_cast<std::size_t>(index)] = static_cast<std::uint8_t>(bitLength >> shift);
    }
    processBlock(block.data());
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (const auto value : hash) out << std::setw(8) << value;
    return out.str();
}

void ensureSpace(const fs::path& source, const fs::path& destination) {
    const auto required = fs::file_size(source) + 64ULL * 1024ULL * 1024ULL;
    const auto available = fs::space(destination.parent_path()).available;
    if (available < required) {
        throw std::runtime_error("Not enough free disk space for the staged ISO.");
    }
}

void copyCancelable(const fs::path& source, const fs::path& destination,
                    const std::atomic<bool>& cancel, const TransactionProgress& progress,
                    const std::string& stage, float progressStart, float progressEnd) {
    std::ifstream input(source, std::ios::binary);
    std::ofstream output(destination, std::ios::binary | std::ios::trunc);
    if (!input || !output) throw std::runtime_error("Cannot create " + destination.string());
    std::vector<char> buffer(4 * 1024 * 1024);
    const std::uint64_t totalSize = fs::file_size(source);
    std::uint64_t copied = 0;
    while (input) {
        if (cancel.load()) throw std::runtime_error("Operation cancelled.");
        input.read(buffer.data(), buffer.size());
        output.write(buffer.data(), input.gcount());
        if (!output) throw std::runtime_error("Failed writing " + destination.string());
        copied += static_cast<std::uint64_t>(input.gcount());
        if (progress && totalSize > 0) {
            const float ratio = static_cast<float>(copied) / static_cast<float>(totalSize);
            progress(stage, progressStart + (progressEnd - progressStart) * ratio);
        }
    }
}

void validateIso(const fs::path& path) {
    tpcm::IsoFileSystem iso = tpcm::IsoFileSystem::open(path.string());
    for (const char* required : {"Z2SoundSeqs.arc", "Z2Sound.baa"}) {
        if (!iso.hasFile(required) || iso.readFile(required).empty()) {
            throw std::runtime_error(std::string("Staged ISO validation failed: missing ") + required);
        }
    }
}

std::string timestamp() {
    const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    std::ostringstream out;
    out << std::put_time(&local, "%Y%m%d-%H%M%S");
    return out.str();
}

fs::path manifestPathFor(const fs::path& iso) {
    return iso.parent_path() / (iso.stem().string() + ".dusk_music_manifest.json");
}

void replaceFile(const fs::path& temporary, const fs::path& finalPath, bool replaceExisting) {
    if (!fs::exists(finalPath)) {
        fs::rename(temporary, finalPath);
        return;
    }
    if (!replaceExisting) throw std::runtime_error("Output already exists: " + finalPath.string());

    const fs::path previous = finalPath.string() + ".previous";
    std::error_code ignored;
    fs::remove(previous, ignored);
    fs::rename(finalPath, previous);
    try {
        fs::rename(temporary, finalPath);
        fs::remove(previous);
    } catch (...) {
        fs::remove(finalPath, ignored);
        fs::rename(previous, finalPath);
        throw;
    }
}

void publishIsoAndManifest(const fs::path& temporaryIso, const fs::path& finalIso,
                           const std::optional<fs::path>& temporaryManifest,
                           bool replaceExisting) {
    const fs::path finalManifest = manifestPathFor(finalIso);
    if (!replaceExisting && (fs::exists(finalIso) || fs::exists(finalManifest))) {
        throw std::runtime_error("Output already exists: " + finalIso.string());
    }

    const fs::path previousIso = finalIso.string() + ".previous";
    const fs::path previousManifest = finalManifest.string() + ".previous";
    std::error_code ignored;
    fs::remove(previousIso, ignored);
    fs::remove(previousManifest, ignored);

    const bool hadIso = fs::exists(finalIso);
    const bool hadManifest = fs::exists(finalManifest);
    if (hadIso) fs::rename(finalIso, previousIso);
    try {
        if (hadManifest) fs::rename(finalManifest, previousManifest);
        fs::rename(temporaryIso, finalIso);
        if (temporaryManifest) fs::rename(*temporaryManifest, finalManifest);
        fs::remove(previousIso, ignored);
        fs::remove(previousManifest, ignored);
    } catch (...) {
        fs::remove(finalIso, ignored);
        fs::remove(finalManifest, ignored);
        if (hadIso && fs::exists(previousIso)) fs::rename(previousIso, finalIso);
        if (hadManifest && fs::exists(previousManifest)) fs::rename(previousManifest, finalManifest);
        throw;
    }
}

}  // namespace

fs::path CustomMusicIsoTransaction::stagedPathFor(const fs::path& source) {
    return source.parent_path() / (source.stem().string() + "-custom-music" + source.extension().string());
}

IsoTransactionResult CustomMusicIsoTransaction::apply(
    const fs::path& source, const IsoMutator& mutator, bool replaceExisting,
    const std::atomic<bool>& cancel, const TransactionLog& log,
    const TransactionProgress& progress) {
    IsoTransactionResult result;
    const fs::path output = stagedPathFor(source);
    const fs::path temporary = output.string() + ".tmp";
    const fs::path manifestPath = manifestPathFor(output);
    const fs::path manifestTemporary = manifestPath.string() + ".tmp";
    try {
        if (!fs::is_regular_file(source)) throw std::runtime_error("Clean source ISO is unavailable.");
        ensureSpace(source, output);
        fs::remove(temporary);
        log("Copying clean source ISO...");
        copyCancelable(source, temporary, cancel, progress, "Copying clean source ISO...", 0.0f, 0.55f);
        log("Applying custom music...");
        if (progress) progress("Applying custom music...", 0.6f);
        const std::string manifest = mutator(temporary, log);
        if (cancel.load()) throw std::runtime_error("Operation cancelled.");
        log("Validating staged ISO...");
        if (progress) progress("Validating staged ISO...", 0.9f);
        validateIso(temporary);
        {
            std::ofstream stream(manifestTemporary, std::ios::trunc);
            if (!stream) throw std::runtime_error("Cannot publish custom music manifest.");
            stream << manifest;
            if (!stream) throw std::runtime_error("Cannot publish custom music manifest.");
        }
        publishIsoAndManifest(temporary, output, manifestTemporary, replaceExisting);
        result = {true, output, manifestPath, "Custom music staged successfully."};
    } catch (const std::exception& e) {
        std::error_code ignored;
        fs::remove(temporary, ignored);
        fs::remove(manifestTemporary, ignored);
        result.message = e.what();
    }
    return result;
}

IsoTransactionResult CustomMusicIsoTransaction::createSnapshot(
    const fs::path& source, const std::atomic<bool>& cancel, const TransactionLog& log,
    const TransactionProgress& progress) {
    IsoTransactionResult result;
    const fs::path snapshot = source.parent_path()
        / (source.stem().string() + "-snapshot-" + timestamp() + source.extension().string());
    const fs::path temporary = snapshot.string() + ".tmp";
    bool snapshotPublished = false;
    try {
        ensureSpace(source, snapshot);
        log("Creating and verifying ISO snapshot...");
        const std::string digest = sha256(
            source, cancel, progress, "Creating ISO snapshot...", 0.0f, 0.95f, temporary);
        replaceFile(temporary, snapshot, false);
        snapshotPublished = true;
        const fs::path metadata = snapshot.string() + ".json";
        const fs::path sourceManifest = manifestPathFor(source);
        const fs::path snapshotManifest = manifestPathFor(snapshot);
        std::string manifestName;
        if (fs::is_regular_file(sourceManifest)) {
            fs::copy_file(sourceManifest, snapshotManifest);
            manifestName = snapshotManifest.filename().string();
        }
        std::ofstream output(metadata);
        output << json({
            {"version", 1},
            {"iso", snapshot.filename().string()},
            {"source", source.string()},
            {"size", fs::file_size(snapshot)},
            {"sha256", digest},
            {"manifest", manifestName},
        }).dump(2);
        if (!output) throw std::runtime_error("Could not write ISO snapshot metadata.");
        result = {true, snapshot, {}, "ISO snapshot created and verified."};
    } catch (const std::exception& e) {
        std::error_code ignored;
        fs::remove(temporary, ignored);
        if (snapshotPublished) {
            fs::remove(snapshot, ignored);
            fs::remove(snapshot.string() + ".json", ignored);
            fs::remove(manifestPathFor(snapshot), ignored);
        }
        result.message = e.what();
    }
    return result;
}

fs::path CustomMusicIsoTransaction::pendingRestorePathFor(const fs::path& target) {
    return target.string() + ".restore-pending";
}

fs::path CustomMusicIsoTransaction::pendingRestoreManifestPathFor(const fs::path& target) {
    return target.string() + ".restore-pending.dusk_music_manifest.json";
}

IsoTransactionResult CustomMusicIsoTransaction::prepareRestore(
    const fs::path& source, const fs::path& target, const std::atomic<bool>& cancel,
    const TransactionLog& log, const TransactionProgress& progress) {
    IsoTransactionResult result;
    const fs::path pending = pendingRestorePathFor(target);
    const fs::path pendingManifest = pendingRestoreManifestPathFor(target);
    try {
        if (!fs::is_regular_file(source)) throw std::runtime_error("Selected restore ISO is unavailable.");
        if (target.empty()) throw std::runtime_error("Dusk has no configured ISO to replace.");
        std::error_code equivalentError;
        if (fs::exists(target) && fs::equivalent(source, target, equivalentError) && !equivalentError) {
            throw std::runtime_error("Selected restore ISO is already the configured ISO.");
        }
        ensureSpace(source, pending);
        std::error_code ignored;
        fs::remove(pending, ignored);
        fs::remove(pendingManifest, ignored);
        log("Preparing ISO restore for next launch...");
        copyCancelable(source, pending, cancel, progress, "Copying selected ISO...", 0.0f, 0.9f);
        if (progress) progress("Validating pending restore...", 0.92f);
        validateIso(pending);
        const fs::path sourceManifest = manifestPathFor(source);
        if (fs::is_regular_file(sourceManifest)) {
            fs::copy_file(sourceManifest, pendingManifest, fs::copy_options::overwrite_existing);
        }
        result = {true, target, {}, "ISO restore prepared. Restart Dusk to apply it."};
    } catch (const std::exception& e) {
        std::error_code ignored;
        fs::remove(pending, ignored);
        fs::remove(pendingManifest, ignored);
        result.message = e.what();
    }
    return result;
}

IsoTransactionResult CustomMusicIsoTransaction::applyPendingRestore(const fs::path& target) {
    IsoTransactionResult result;
    const fs::path pending = pendingRestorePathFor(target);
    const fs::path pendingManifest = pendingRestoreManifestPathFor(target);
    if (!fs::is_regular_file(pending)) return result;
    try {
        validateIso(pending);
        if (fs::is_regular_file(pendingManifest)) {
            publishIsoAndManifest(pending, target, pendingManifest, true);
        } else {
            publishIsoAndManifest(pending, target, std::nullopt, true);
        }
        result = {true, target, {}, "Pending ISO restore applied."};
    } catch (const std::exception& e) {
        result.message = e.what();
    }
    return result;
}

}  // namespace dusk::custom_music
