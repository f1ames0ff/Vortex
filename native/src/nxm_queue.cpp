#include "vortex/core/nxm_queue.hpp"

#include "vortex/deployment/platform.hpp"

#include <algorithm>

namespace vortex::core {

NxmQueueEntry NxmQueue::add(NxmQueueEntry entry) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto existing = std::find_if(entries_.begin(), entries_.end(),
                                       [&](const auto& candidate) {
                                           return candidate.input == entry.input;
                                       });
    if (existing != entries_.end()) {
        existing->gameDomain = std::move(entry.gameDomain);
        existing->type = std::move(entry.type);
        existing->modId = entry.modId;
        existing->fileId = entry.fileId;
        existing->collectionSlug = std::move(entry.collectionSlug);
        return *existing;
    }
    entry.addedAt = deployment::platform::now_ms();
    entries_.push_back(entry);
    return entry;
}

std::vector<NxmQueueEntry> NxmQueue::list() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return entries_;
}

bool NxmQueue::remove(const std::string& input) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto count = std::erase_if(entries_, [&](const auto& entry) {
        return entry.input == input;
    });
    return count > 0;
}

void NxmQueue::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    entries_.clear();
}

std::optional<NxmQueueEntry> NxmQueue::updateAwaiting(const std::string& input,
                                                      bool awaiting) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto entry = std::find_if(entries_.begin(), entries_.end(),
                                    [&](const auto& candidate) {
                                        return candidate.input == input;
                                    });
    if (entry == entries_.end()) {
        return std::nullopt;
    }
    entry->awaitingLink = awaiting;
    return *entry;
}

std::optional<NxmQueueEntry> NxmQueue::matchAwaited(const std::string& gameDomain, int modId,
                                                    int fileId) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto entry = std::find_if(entries_.begin(), entries_.end(),
                                    [&](const auto& candidate) {
                                        return candidate.awaitingLink &&
                                               candidate.gameDomain == gameDomain &&
                                               candidate.modId == modId &&
                                               candidate.fileId == fileId;
                                    });
    if (entry == entries_.end()) {
        return std::nullopt;
    }
    return *entry;
}

} // namespace vortex::core
