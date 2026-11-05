#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace vortex::core {

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
};

class CollectionJobs {
public:
    std::string start(CollectionJob job);
    CollectionJob* find(const std::string& jobId);
    const CollectionJob* find(const std::string& jobId) const;

    void pause(const std::string& jobId);
    void cancel(const std::string& jobId);
    void update(const std::string& jobId, const CollectionMemberUpdate& update);

    // Selects up to the job's concurrency from the allowed phase, marks them
    // queued, and advances the phase when the current one is settled.
    std::vector<CollectionMemberState> next(const std::string& jobId);

    static nlohmann::json status(const CollectionJob& job);

private:
    std::map<std::string, CollectionJob> jobs_;
};

bool collectionMemberTerminal(const std::string& status);
bool collectionMemberActive(const std::string& status);

} // namespace vortex::core
