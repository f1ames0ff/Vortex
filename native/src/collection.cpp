#include "vortex/core/collection.hpp"

#include "vortex/deployment/platform.hpp"

#include <algorithm>
#include <set>

using nlohmann::json;

namespace vortex::core {

namespace {

bool isKnownStatus(const std::string& status) {
    static const std::set<std::string> known = {
        "pending", "queued", "downloading", "downloaded",
        "installing", "installed", "failed", "ignored",
    };
    return known.contains(status);
}

void advancePhase(CollectionJob& job) {
    std::set<int> phases;
    for (const auto& member : job.members) {
        phases.insert(member.phase);
    }
    if (phases.empty()) {
        job.allowedPhase = 0;
        return;
    }

    for (;;) {
        bool currentHasNonTerminal = false;
        for (const auto& member : job.members) {
            if (member.phase == job.allowedPhase &&
                !collectionMemberTerminal(member.status)) {
                currentHasNonTerminal = true;
                break;
            }
        }
        if (currentHasNonTerminal) {
            return;
        }

        std::optional<int> nextPhase;
        for (const auto phase : phases) {
            if (phase <= job.allowedPhase) {
                continue;
            }
            const bool hasWork = std::any_of(
                job.members.begin(), job.members.end(), [&](const auto& member) {
                    return member.phase == phase && !collectionMemberTerminal(member.status);
                });
            if (hasWork) {
                nextPhase = phase;
                break;
            }
        }
        if (!nextPhase.has_value()) {
            return;
        }
        job.allowedPhase = *nextPhase;
    }
}

std::size_t statusCount(const CollectionJob& job, const std::string& status) {
    return static_cast<std::size_t>(std::count_if(
        job.members.begin(), job.members.end(),
        [&](const auto& member) { return member.status == status; }));
}

} // namespace

bool collectionMemberTerminal(const std::string& status) {
    return status == "installed" || status == "failed" || status == "ignored";
}

bool collectionMemberActive(const std::string& status) {
    return status == "queued" || status == "downloading" || status == "installing";
}

std::string CollectionJobs::start(CollectionJob job) {
    if (job.id.empty()) {
        job.id = "collection_" + deployment::platform::operation_id();
    }
    job.startedAt = deployment::platform::now_ms();
    job.lastProgress = job.startedAt;
    if (!job.members.empty()) {
        job.allowedPhase = job.members.front().phase;
        for (const auto& member : job.members) {
            job.allowedPhase = std::min(job.allowedPhase, member.phase);
        }
    }
    const auto id = job.id;
    jobs_.insert_or_assign(id, std::move(job));
    return id;
}

CollectionJob* CollectionJobs::find(const std::string& jobId) {
    const auto found = jobs_.find(jobId);
    return found == jobs_.end() ? nullptr : &found->second;
}

const CollectionJob* CollectionJobs::find(const std::string& jobId) const {
    const auto found = jobs_.find(jobId);
    return found == jobs_.end() ? nullptr : &found->second;
}

void CollectionJobs::pause(const std::string& jobId) {
    if (auto* job = find(jobId)) {
        job->paused = true;
        job->lastProgress = deployment::platform::now_ms();
    }
}

void CollectionJobs::cancel(const std::string& jobId) {
    if (auto* job = find(jobId)) {
        job->cancelled = true;
        job->lastProgress = deployment::platform::now_ms();
    }
}

void CollectionJobs::update(const std::string& jobId, const CollectionMemberUpdate& update) {
    auto* job = find(jobId);
    if (job == nullptr) {
        return;
    }
    const auto member = std::find_if(job->members.begin(), job->members.end(),
                                     [&](const auto& candidate) {
                                         return candidate.ruleId == update.ruleId;
                                     });
    if (member == job->members.end()) {
        return;
    }

    const auto status = isKnownStatus(update.status) ? update.status : "failed";
    member->error = isKnownStatus(update.status) ? update.error : "invalid collection status";
    if (status == "failed") {
        ++member->attempts;
        if (update.retry && member->attempts < job->maxRetries) {
            member->status = "pending";
        } else {
            member->status = "failed";
        }
    } else {
        member->status = status;
    }
    if (update.modId.has_value()) {
        member->modId = update.modId;
    }
    if (update.downloadId.has_value()) {
        member->downloadId = update.downloadId;
    }

    job->lastProgress = deployment::platform::now_ms();
    advancePhase(*job);
}

std::vector<CollectionMemberState> CollectionJobs::next(const std::string& jobId) {
    auto* job = find(jobId);
    if (job == nullptr || job->paused || job->cancelled) {
        return {};
    }

    advancePhase(*job);
    std::set<int> phases;
    for (const auto& member : job->members) {
        phases.insert(member.phase);
    }

    for (std::size_t attempt = 0; attempt <= phases.size(); ++attempt) {
        const auto active = std::count_if(
            job->members.begin(), job->members.end(), [&](const auto& member) {
                return member.phase == job->allowedPhase &&
                       collectionMemberActive(member.status);
            });
        const auto capacity =
            job->concurrency <= 0 ? job->members.size() : static_cast<std::size_t>(job->concurrency);
        if (static_cast<std::size_t>(active) >= capacity) {
            return {};
        }

        std::vector<CollectionMemberState> selected;
        const auto available = capacity - static_cast<std::size_t>(active);
        for (auto& member : job->members) {
            if (selected.size() >= available) {
                break;
            }
            if (member.phase != job->allowedPhase ||
                (member.status != "pending" && member.status != "downloaded")) {
                continue;
            }
            member.status = "queued";
            selected.push_back(member);
        }
        if (!selected.empty()) {
            job->lastProgress = deployment::platform::now_ms();
            return selected;
        }
        if (active > 0) {
            return {};
        }

        // The current phase has no active or runnable members. advancePhase
        // has already skipped settled phases; if it didn't move, there is no
        // work left.
        const auto previous = job->allowedPhase;
        advancePhase(*job);
        if (job->allowedPhase == previous) {
            break;
        }
    }
    return {};
}

json CollectionJobs::status(const CollectionJob& job) {
    json members = json::array();
    std::set<int> phases;
    for (const auto& member : job.members) {
        phases.insert(member.phase);
        json value = {
            {"ruleIndex", member.ruleIndex},
            {"ruleId", member.ruleId},
            {"type", member.type},
            {"phase", member.phase},
            {"ignored", member.ignored},
            {"status", member.status},
            {"attempts", member.attempts},
        };
        if (member.modId.has_value()) {
            value["modId"] = *member.modId;
        }
        if (member.downloadId.has_value()) {
            value["downloadId"] = *member.downloadId;
        }
        if (!member.error.empty()) {
            value["error"] = member.error;
        }
        members.push_back(std::move(value));
    }

    const auto now = deployment::platform::now_ms();
    const bool complete = std::all_of(job.members.begin(), job.members.end(),
                                      [](const auto& member) {
                                          return collectionMemberTerminal(member.status);
                                      });
    const bool stalled =
        !complete && !job.paused && !job.cancelled &&
        (now - job.lastProgress) > job.stallMs;

    json phaseList = json::array();
    for (const auto phase : phases) {
        phaseList.push_back(phase);
    }

    json counts = json::object();
    for (const auto* status : {"pending", "queued", "downloading", "downloaded",
                               "installing", "installed", "failed", "ignored"}) {
        counts[status] = statusCount(job, status);
    }

    return {
        {"jobId", job.id},
        {"collectionId", job.collectionId},
        {"gameId", job.gameId},
        {"phase", job.allowedPhase},
        {"phases", std::move(phaseList)},
        {"members", std::move(members)},
        {"counts", std::move(counts)},
        {"complete", complete},
        {"paused", job.paused},
        {"cancelled", job.cancelled},
        {"stalled", stalled},
        {"startedAt", job.startedAt},
        {"lastProgress", job.lastProgress},
    };
}

} // namespace vortex::core
