#pragma once

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace vortex::core {

struct NxmQueueEntry {
    std::string input;
    std::string gameDomain;
    std::string type = "mod";
    int modId = 0;
    int fileId = 0;
    std::string collectionSlug;
    bool awaitingLink = false;
    std::int64_t addedAt = 0;
};

/**
 * Owns the free-user NXM queue. The renderer still renders the dialog and
 * performs the browser round-trip, but ordering, deduplication, cancellation
 * and "which parked link is this?" matching live here.
 */
class NxmQueue {
public:
    NxmQueue() = default;
    NxmQueue(const NxmQueue&) = delete;
    NxmQueue& operator=(const NxmQueue&) = delete;

    NxmQueueEntry add(NxmQueueEntry entry);
    std::vector<NxmQueueEntry> list() const;
    bool remove(const std::string& input);
    void clear();
    std::optional<NxmQueueEntry> updateAwaiting(const std::string& input, bool awaiting);
    std::optional<NxmQueueEntry> matchAwaited(const std::string& gameDomain, int modId,
                                              int fileId) const;

private:
    mutable std::mutex mutex_;
    std::vector<NxmQueueEntry> entries_;
};

} // namespace vortex::core
