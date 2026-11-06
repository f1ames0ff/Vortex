#include "vortex/core/collection.hpp"

#include "vortex/core/archive.hpp"
#include "vortex/core/process.hpp"
#include "vortex/deployment/platform.hpp"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <set>
#include <sstream>
#include <thread>

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

std::vector<std::string> listTree(const std::filesystem::path& root) {
    std::vector<std::string> result;
    std::error_code ec;
    std::filesystem::recursive_directory_iterator iterator(
        root, std::filesystem::directory_options::skip_permission_denied, ec);
    if (ec) {
        throw std::runtime_error("list extracted archive: " + ec.message());
    }
    for (const auto& entry : iterator) {
        if (entry.is_directory()) {
            continue;
        }
        const auto rel = std::filesystem::relative(entry.path(), root, ec);
        if (ec) {
            continue;
        }
        result.push_back(rel.generic_string());
    }
    std::sort(result.begin(), result.end());
    return result;
}

std::vector<InstallInstruction> resolveFomod(const CollectionFomodTask& fomod,
                                             const std::filesystem::path& extracted) {
    json files = json::array();
    for (const auto& file : listTree(extracted)) {
        files.push_back(file);
    }
    json params = {
        {"nativeLibrary", fomod.nativeLibrary},
        {"files", std::move(files)},
        {"stopPatterns", json::array()},
        {"validate", false},
    };
    if (!fomod.pluginPath.empty()) {
        params["pluginPath"] = fomod.pluginPath;
    }
    if (!fomod.scriptPath.empty()) {
        params["scriptPath"] = fomod.scriptPath;
    }
    if (!fomod.preset.empty()) {
        params["preset"] = json::parse(fomod.preset);
    }
    const json request = {{"method", "fomod.resolve"}, {"params", params}};

    const auto requestPath =
        std::filesystem::temp_directory_path() /
        std::filesystem::path("vortex-fomod-" +
                              std::to_string(
                                  std::chrono::steady_clock::now().time_since_epoch().count()) +
                              ".json");
    {
        std::ofstream output(requestPath, std::ios::binary | std::ios::trunc);
        if (!output) {
            throw std::runtime_error("failed to create FOMOD request");
        }
        output << request.dump();
        output.flush();
        if (!output) {
            throw std::runtime_error("failed to write FOMOD request");
        }
    }

    const auto process = runProcess(
        std::filesystem::path(fomod.node), {fomod.script, "--request", requestPath.string()},
        nullptr, {{"ELECTRON_RUN_AS_NODE", "1"}});
    std::error_code cleanup;
    std::filesystem::remove(requestPath, cleanup);
    if (process.exitCode != 0) {
        throw std::runtime_error(process.output.empty() ? "FOMOD helper failed" : process.output);
    }
    const auto start = process.output.find_first_of('{');
    if (start == std::string::npos) {
        throw std::runtime_error("FOMOD helper produced no JSON");
    }
    const auto response = json::parse(process.output.substr(start));
    if (response.contains("error")) {
        throw std::runtime_error(response["error"].get<std::string>());
    }
    const auto result = response.value("result", json::object());
    std::vector<InstallInstruction> instructions;
    if (result.contains("instructions") && result["instructions"].is_array()) {
        for (const auto& instruction : result["instructions"]) {
            if (!instruction.is_object() || instruction.value("type", "") != "copy") {
                continue;
            }
            InstallInstruction copy;
            copy.type = InstallInstructionType::Copy;
            copy.source = instruction.value("source", std::string{});
            copy.destination = instruction.value("destination", std::string{});
            if (!copy.source.empty() && !copy.destination.empty()) {
                instructions.push_back(std::move(copy));
            }
        }
    }
    return instructions;
}

std::string joinFailures(const std::vector<InstallFailure>& failures) {
    std::ostringstream stream;
    std::size_t count = 0;
    for (const auto& failure : failures) {
        if (count++ > 0) {
            stream << "; ";
        }
        stream << failure.destination << ": " << failure.message;
        if (count >= 5) {
            stream << "; ...";
            break;
        }
    }
    return stream.str();
}

} // namespace

bool collectionMemberTerminal(const std::string& status) {
    return status == "installed" || status == "failed" || status == "ignored";
}

bool collectionMemberActive(const std::string& status) {
    return status == "queued" || status == "downloading" || status == "installing";
}

CollectionJobs::CollectionJobs() = default;

CollectionJobs::~CollectionJobs() {
    stopWorkers();
}

void CollectionJobs::stopWorkers() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
    }
    wakeup_.notify_all();
    std::map<std::string, std::thread> runners;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        runners.swap(runners_);
    }
    for (auto& entry : runners) {
        if (entry.second.joinable()) {
            entry.second.join();
        }
    }
}

std::string CollectionJobs::start(CollectionJob job) {
    std::lock_guard<std::mutex> lock(mutex_);
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

bool CollectionJobs::copy(const std::string& jobId, CollectionJob& result) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = jobs_.find(jobId);
    if (found == jobs_.end()) {
        return false;
    }
    result = found->second;
    return true;
}

void CollectionJobs::pause(const std::string& jobId) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = jobs_.find(jobId);
        if (found == jobs_.end()) {
            return;
        }
        auto& job = found->second;
        job.paused = true;
        job.lastProgress = deployment::platform::now_ms();
        // A worker may have exited with members still marked active. Make them
        // runnable again so a later run/resume can pick them up.
        for (auto& member : job.members) {
            if (collectionMemberActive(member.status)) {
                member.status = "pending";
            }
        }
    }
    wakeup_.notify_all();
}

void CollectionJobs::cancel(const std::string& jobId) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = jobs_.find(jobId);
        if (found == jobs_.end()) {
            return;
        }
        auto& job = found->second;
        job.cancelled = true;
        job.lastProgress = deployment::platform::now_ms();
        for (auto& member : job.members) {
            if (!collectionMemberTerminal(member.status)) {
                member.status = "failed";
                if (member.error.empty()) {
                    member.error = "collection cancelled";
                }
            }
        }
    }
    wakeup_.notify_all();
}

void CollectionJobs::touch(const std::string& jobId) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = jobs_.find(jobId);
    if (found != jobs_.end()) {
        found->second.lastProgress = deployment::platform::now_ms();
    }
}

void CollectionJobs::update(const std::string& jobId, const CollectionMemberUpdate& update) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = jobs_.find(jobId);
    if (found == jobs_.end()) {
        return;
    }
    auto& job = found->second;
    const auto member = std::find_if(job.members.begin(), job.members.end(),
                                     [&](const auto& candidate) {
                                         return candidate.ruleId == update.ruleId;
                                     });
    if (member == job.members.end()) {
        return;
    }

    const auto status = isKnownStatus(update.status) ? update.status : "failed";
    member->error = isKnownStatus(update.status) ? update.error : "invalid collection status";
    if (status == "failed") {
        ++member->attempts;
        if (update.retry && member->attempts < job.maxRetries) {
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
    if (update.result.has_value() && member->status == "installed") {
        member->result = update.result;
    } else if (member->status == "failed" || member->status == "pending") {
        member->result.reset();
    }

    job.lastProgress = deployment::platform::now_ms();
    advancePhase(job);
}

std::vector<CollectionMemberState> CollectionJobs::next(const std::string& jobId) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = jobs_.find(jobId);
    if (found == jobs_.end()) {
        return {};
    }
    auto& job = found->second;
    if (job.paused || job.cancelled) {
        return {};
    }

    advancePhase(job);
    std::set<int> phases;
    for (const auto& member : job.members) {
        phases.insert(member.phase);
    }

    for (std::size_t attempt = 0; attempt <= phases.size(); ++attempt) {
        const auto active = std::count_if(
            job.members.begin(), job.members.end(), [&](const auto& member) {
                return member.phase == job.allowedPhase &&
                       collectionMemberActive(member.status);
            });
        const auto capacity =
            job.concurrency <= 0 ? job.members.size() : static_cast<std::size_t>(job.concurrency);
        if (static_cast<std::size_t>(active) >= capacity) {
            return {};
        }

        std::vector<CollectionMemberState> selected;
        const auto available = capacity - static_cast<std::size_t>(active);
        for (auto& member : job.members) {
            if (selected.size() >= available) {
                break;
            }
            if (member.phase != job.allowedPhase ||
                (member.status != "pending" && member.status != "downloaded")) {
                continue;
            }
            member.status = "queued";
            selected.push_back(member);
        }
        if (!selected.empty()) {
            job.lastProgress = deployment::platform::now_ms();
            return selected;
        }
        if (active > 0) {
            return {};
        }

        const auto previous = job.allowedPhase;
        advancePhase(job);
        if (job.allowedPhase == previous) {
            break;
        }
    }
    return {};
}

void CollectionJobs::executeMember(const std::string& jobId, const CollectionMemberState& member,
                                   DownloadQueue& downloads) {
    try {
        if (!member.task.download.has_value() && !member.task.install.has_value()) {
            throw std::runtime_error("collection member has no execution task");
        }

        std::filesystem::path archive;
        std::optional<std::string> downloadId;
        if (member.task.download.has_value()) {
            auto request = *member.task.download;
            request.id = jobId + ":" + member.ruleId;
            downloadId = request.id;

            update(jobId, {member.ruleId, "downloading", std::nullopt, downloadId, "", false, std::nullopt});
            try {
                downloads.add(request);
            } catch (const std::exception&) {
                // A retry reuses the same id; resume if the previous attempt is
                // still tracked, otherwise surface the add error.
                if (!downloads.status(*downloadId).has_value()) {
                    throw;
                }
                downloads.resume(*downloadId);
            }

            std::uint64_t lastBytes = 0;
            auto lastChange = std::chrono::steady_clock::now();
            for (;;) {
                CollectionJob snapshot;
                if (!copy(jobId, snapshot)) {
                    throw std::runtime_error("collection job disappeared");
                }
                if (snapshot.cancelled || snapshot.paused) {
                    downloads.cancel(*downloadId);
                    return;
                }

                const auto status = downloads.status(*downloadId);
                if (!status.has_value()) {
                    throw std::runtime_error("download disappeared from the core queue");
                }
                if (status->bytes != lastBytes) {
                    lastBytes = status->bytes;
                    lastChange = std::chrono::steady_clock::now();
                    touch(jobId);
                }
                if (status->state == "finished") {
                    archive = request.directory / std::filesystem::path(request.fileName);
                    break;
                }
                if (status->state == "failed" || status->state == "cancelled") {
                    throw std::runtime_error(status->error.empty() ? "download failed"
                                                                   : status->error);
                }

                const auto stallMs = snapshot.stallMs > 0 ? snapshot.stallMs : 60000;
                if (std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - lastChange)
                        .count() > static_cast<long long>(stallMs) * 2) {
                    downloads.cancel(*downloadId);
                    throw std::runtime_error("download stalled");
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }

        if (member.task.extract.has_value()) {
            auto extract = *member.task.extract;
            if (extract.archive.empty()) {
                if (archive.empty()) {
                    throw std::runtime_error("archive path is required for extraction");
                }
                extract.archive = archive;
            }
            const auto result =
                extractArchive(extract.archive, extract.destination, extract.sevenZip,
                               extract.password);
            if (result.code != 0 || !result.errors.empty()) {
                throw std::runtime_error(
                    result.errors.empty() ? "archive extraction failed" : result.errors.front());
            }
        }

        std::optional<std::string> installationPath;
        std::vector<std::string> installedFiles;
        if (member.task.install.has_value() || member.task.copyAll ||
            member.task.fomod.has_value()) {
            InstallRequest install =
                member.task.install.value_or(InstallRequest{});
            if (install.tempPath.empty() && member.task.extract.has_value()) {
                install.tempPath = member.task.extract->destination;
            }
            if (install.destinationPath.empty()) {
                throw std::runtime_error("install.destination is required");
            }
            if (member.task.copyAll) {
                for (const auto& file : listTree(install.tempPath)) {
                    install.instructions.push_back(
                        {InstallInstructionType::Copy, file, file, "", false});
                }
            }
            if (member.task.fomod.has_value()) {
                const auto resolved = resolveFomod(*member.task.fomod, install.tempPath);
                install.instructions.insert(install.instructions.end(), resolved.begin(),
                                            resolved.end());
            }
            if (install.instructions.empty()) {
                throw std::runtime_error("collection task produced no install instructions");
            }
            install.operationId = jobId + "-" + member.ruleId;
            if (install.journalPath.empty()) {
                install.journalPath = install.destinationPath.parent_path() /
                                      ".vortex-install-journal" /
                                      (install.operationId + ".jsonl");
            }
            installationPath = install.destinationPath.string();
            for (const auto& instruction : install.instructions) {
                installedFiles.push_back(instruction.destination);
            }
            const auto report = applyInstall(install);
            if (!report.failures.empty()) {
                throw std::runtime_error("install failed: " + joinFailures(report.failures));
            }
        }

        CollectionMemberResult result;
        result.modId = member.task.modId.value_or("");
        if (installationPath.has_value()) {
            result.installationPath = *installationPath;
        }
        result.downloadId = downloadId;
        if (!archive.empty()) {
            result.archivePath = archive.string();
        }
        result.files = std::move(installedFiles);
        update(jobId, {member.ruleId,
                       "installed",
                       member.task.modId,
                       std::nullopt,
                       "",
                       false,
                       std::move(result)});
    } catch (const std::exception& err) {
        update(jobId, {member.ruleId, "failed", std::nullopt, std::nullopt, err.what(), true, std::nullopt});
    } catch (...) {
        update(jobId, {member.ruleId, "failed", std::nullopt, std::nullopt,
                       "unknown collection task error", true, std::nullopt});
    }
}

bool CollectionJobs::setTask(const std::string& jobId, const std::string& ruleId,
                             CollectionTask task) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = jobs_.find(jobId);
    if (found == jobs_.end()) {
        return false;
    }
    const auto member = std::find_if(found->second.members.begin(), found->second.members.end(),
                                     [&](const auto& candidate) {
                                         return candidate.ruleId == ruleId;
                                     });
    if (member == found->second.members.end()) {
        return false;
    }
    member->task = std::move(task);
    found->second.lastProgress = deployment::platform::now_ms();
    return true;
}

bool CollectionJobs::execute(const std::string& jobId, const std::string& ruleId,
                             DownloadQueue& downloads, CollectionJob& result) {
    CollectionMemberState member;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = jobs_.find(jobId);
        if (found == jobs_.end()) {
            return false;
        }
        const auto entry = std::find_if(found->second.members.begin(), found->second.members.end(),
                                        [&](const auto& candidate) {
                                            return candidate.ruleId == ruleId;
                                        });
        if (entry == found->second.members.end()) {
            return false;
        }
        member = *entry;
        entry->status = "queued";
    }

    executeMember(jobId, member, downloads);
    return copy(jobId, result);
}

void CollectionJobs::run(const std::string& jobId, DownloadQueue& downloads) {
    std::thread previous;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = jobs_.find(jobId);
        if (found == jobs_.end()) {
            return;
        }
        const auto runner = runners_.find(jobId);
        if (runner != runners_.end()) {
            if (runner->second.joinable()) {
                // A completed worker is joined before restarting; a live one
                // means this job is already being driven.
                if (!found->second.paused && !found->second.cancelled) {
                    return;
                }
            }
            previous = std::move(runner->second);
        }
        found->second.paused = false;
        found->second.cancelled = false;
    }
    if (previous.joinable()) {
        previous.join();
    }

    std::thread worker([this, &downloads, jobId] {
        for (;;) {
            CollectionJob snapshot;
            if (!copy(jobId, snapshot) || stopping_) {
                return;
            }
            if (snapshot.cancelled || snapshot.paused) {
                return;
            }
            const bool complete =
                std::all_of(snapshot.members.begin(), snapshot.members.end(),
                            [](const auto& member) {
                                return collectionMemberTerminal(member.status);
                            });
            if (complete) {
                return;
            }

            const auto selected = next(jobId);
            if (selected.empty()) {
                std::unique_lock<std::mutex> lock(mutex_);
                wakeup_.wait_for(lock, std::chrono::milliseconds(100));
                continue;
            }

            std::vector<std::thread> batch;
            batch.reserve(selected.size());
            for (const auto& member : selected) {
                batch.emplace_back(&CollectionJobs::executeMember, this, jobId, member,
                                   std::ref(downloads));
            }
            for (auto& thread : batch) {
                thread.join();
            }
        }
    });

    {
        std::lock_guard<std::mutex> lock(mutex_);
        runners_[jobId] = std::move(worker);
    }
    wakeup_.notify_all();
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
            {"hasTask", member.task.download.has_value() || member.task.extract.has_value() ||
                            member.task.install.has_value()},
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
        if (member.result.has_value()) {
            json result = {
                {"modId", member.result->modId},
                {"installationPath", member.result->installationPath},
                {"files", member.result->files},
            };
            if (member.result->downloadId.has_value()) {
                result["downloadId"] = *member.result->downloadId;
            }
            if (member.result->archivePath.has_value()) {
                result["archivePath"] = *member.result->archivePath;
            }
            value["result"] = std::move(result);
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
