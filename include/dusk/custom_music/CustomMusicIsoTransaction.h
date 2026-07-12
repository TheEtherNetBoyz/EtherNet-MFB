#pragma once

#include <atomic>
#include <filesystem>
#include <functional>
#include <string>

namespace dusk::custom_music {

struct IsoTransactionResult {
    bool success = false;
    std::filesystem::path outputIso;
    std::filesystem::path manifestPath;
    std::string message;
};

using TransactionLog = std::function<void(const std::string&)>;
using TransactionProgress = std::function<void(const std::string&, float)>;
using IsoMutator = std::function<std::string(const std::filesystem::path&, const TransactionLog&)>;

class CustomMusicIsoTransaction {
public:
    static std::filesystem::path stagedPathFor(const std::filesystem::path& cleanSourceIso);
    static IsoTransactionResult apply(
        const std::filesystem::path& cleanSourceIso, const IsoMutator& mutator,
        bool replaceExisting, const std::atomic<bool>& cancel, const TransactionLog& log,
        const TransactionProgress& progress);
    static IsoTransactionResult createSnapshot(
        const std::filesystem::path& sourceIso, const std::atomic<bool>& cancel,
        const TransactionLog& log, const TransactionProgress& progress);
    static std::filesystem::path pendingRestorePathFor(const std::filesystem::path& targetIso);
    static std::filesystem::path pendingRestoreManifestPathFor(
        const std::filesystem::path& targetIso);
    static IsoTransactionResult prepareRestore(
        const std::filesystem::path& sourceIso, const std::filesystem::path& targetIso,
        const std::atomic<bool>& cancel, const TransactionLog& log,
        const TransactionProgress& progress);
    static IsoTransactionResult applyPendingRestore(const std::filesystem::path& targetIso);
};

}  // namespace dusk::custom_music
