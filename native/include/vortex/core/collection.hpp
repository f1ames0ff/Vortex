#pragma once

#include "vortex/core/download.hpp"
#include "vortex/core/install.hpp"

#include <nlohmann/json.hpp>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace vortex::core {

struct CollectionExtractTask {
    std::filesystem::path archive;
    std::filesystem::path destination;
    std::filesystem::path sevenZip;
    std::string password;
};

struct CollectionFomodTask {
    std::string nativeLibrary;
    std::string node;
    std::string script;
    std::string preset;
    std::string pluginPath;
    std::string scriptPath;
};

struct CollectionTask {
    std::optional<DownloadRequest> download;
    std::optional<CollectionExtractTask> extract;
    std::optional<InstallRequest> install;
    std::optional<CollectionFomodTask> fomod;
    bool copyAll = false;
    std::optional<std::string> modId;
};

struct CollectionMemberResult {
    std::string modId;
    std::string installationPath;
    std::optional<std::string> downloadId;
    std::optional<std::string> archivePath;
    std::vector<std::string> files;
};

struct CollectionMemberState {
    int ruleIndex = 0;
    std::string ruleId;
    std::string type;
    int phase = 0;
    bool ignored = false;
    std::string status = "pending";
    std::optional<std::string> modId;
    std::optional<std::string> downloadId;
    std::string error;
    int attempts = 0;
    bool fallback = false;
    CollectionTask task;
    std::optional<CollectionMemberResult> result;
};

struct CollectionJob {
    std::string id;
    std::string collectionId;
    std::string gameId;
    int concurrency = 5;
    int maxRetries = 3;
    int stallMs = 60000;
    int allowedPhase = 0;
    bool paused = false;
    bool cancelled = false;
    std::int64_t startedAt = 0;
    std::int64_t lastProgress = 0;
    std::vector<CollectionMemberState> members;
};

struct CollectionMemberUpdate {
    std::string ruleId;
    std::string status;
    std::optional<std::string> modId;
    std::optional<std::string> downloadId;
    std::string error;
    bool retry = false;
    std::optional<CollectionMemberResult> result;
};

class CollectionJobs {
public:
    CollectionJobs();
    ~CollectionJobs();

    CollectionJobs(const CollectionJobs&) = delete;
    CollectionJobs& operator=(const CollectionJobs&) = delete;

    std::string start(CollectionJob job);
    CollectionJob* find(const std::string& jobId);
    const CollectionJob* find(const std::string& jobId) const;
    bool copy(const std::string& jobId, CollectionJob& result) const;

    void pause(const std::string& jobId);
    void cancel(const std::string& jobId);
    void update(const std::string& jobId, const CollectionMemberUpdate& update);
    void touch(const std::string& jobId);

    // Selects up to the job's concurrency from the allowed phase, marks them
    // queued, and advances the phase when the current one is settled. With
    // taskOnly, taskless members are left pending for the TypeScript fallback.
    std::vector<CollectionMemberState> next(const std::string& jobId, bool taskOnly = false);

    /**
     * Starts a worker that repeatedly takes members from `next` and executes
     * their download/extract/install task. This is the core-driven counterpart
     * to the renderer calling `next`/`member` itself.
     */
    void run(const std::string& jobId, DownloadQueue& downloads, bool taskOnly = true);

    /** Attach or replace the execution task of one member. */
    bool setTask(const std::string& jobId, const std::string& ruleId, CollectionTask task);

    /**
     * Execute one member synchronously (download -> extract -> install) and
     * return the resulting job state. Used by renderer orchestrators that still
     * drive phases themselves.
     */
    bool execute(const std::string& jobId, const std::string& ruleId, DownloadQueue& downloads,
                 CollectionJob& result);

    static nlohmann::json status(const CollectionJob& job);

private:
    void executeMember(const std::string& jobId, const CollectionMemberState& member,
                       DownloadQueue& downloads);
    void handOffToFallback(const std::string& jobId, const std::string& ruleId);
    void stopWorkers();

    mutable std::mutex mutex_;
    std::condition_variable wakeup_;
    std::map<std::string, CollectionJob> jobs_;
    std::map<std::string, std::thread> runners_;
    std::atomic<bool> stopping_{false};
};

bool collectionMemberTerminal(const std::string& status);
bool collectionMemberActive(const std::string& status);

} // namespace vortex::core
