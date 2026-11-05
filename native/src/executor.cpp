#include "vortex/deployment/executor.hpp"

#include "vortex/deployment/manifest.hpp"
#include "vortex/deployment/platform.hpp"
#include "vortex/deployment/planner.hpp"

#include <map>
#include <system_error>

namespace fs = std::filesystem;
using nlohmann::json;

namespace vortex::deployment {

namespace {

fs::path sourcePath(const ExecutorContext& context, const DeployedFile& file) {
    return context.stagingPath / fs::path(file.source) / fs::path(file.relPath);
}

fs::path outputPath(const ExecutorContext& context, const DeployedFile& file) {
    fs::path result = context.targetPath;
    if (file.target.has_value() && !file.target->empty()) {
        result /= fs::path(*file.target);
    }
    return result / fs::path(file.relPath);
}

bool pathExists(const fs::path& path) {
    std::error_code ec;
    return fs::exists(path, ec);
}

void ensureParent(const fs::path& path) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    if (ec) {
        throw std::system_error(ec, "create output directory");
    }
}

json fileDetails(const DeployedFile& file, const fs::path& output) {
    return {
        {"source", file.source},
        {"relPath", file.relPath},
        {"output", output.string()},
    };
}

void recordFailure(ApplyReport& report, const DeployedFile& file, const fs::path& output,
                   const std::string& message) {
    report.failures.push_back({file.source, file.relPath, output.string(), message});
}

bool removeEntry(const ExecutorContext& context, const DeployedFile& file, bool restoreBackup,
                 ApplyReport& report, Journal& journal) {
    const auto output = outputPath(context, file);
    const auto source = sourcePath(context, file);
    const auto backup = platform::backup_path(output);

    if (!pathExists(output)) {
        if (restoreBackup && pathExists(backup)) {
            try {
                fs::rename(backup, output);
                ++report.restored;
                journal.event("file.restore", fileDetails(file, output));
            } catch (const std::exception& err) {
                recordFailure(report, file, output, err.what());
                return false;
            }
        }
        return true;
    }

    if (!platform::is_hardlink_to(source, output)) {
        recordFailure(report, file, output,
                      "the target was changed outside Vortex; refusing to remove it");
        return false;
    }

    try {
        fs::remove(output);
        ++report.removed;
        journal.event("file.unlink", fileDetails(file, output));
        platform::remove_empty_parents(output.parent_path(), context.targetPath);

        if (restoreBackup && pathExists(backup)) {
            fs::rename(backup, output);
            ++report.restored;
            journal.event("file.restore", fileDetails(file, output));
        }
        return true;
    } catch (const std::exception& err) {
        recordFailure(report, file, output, err.what());
        return false;
    }
}

bool linkEntry(const ExecutorContext& context, const DeployedFile& file, ApplyReport& report,
               Journal& journal) {
    const auto source = sourcePath(context, file);
    const auto output = outputPath(context, file);
    const auto backup = platform::backup_path(output);

    if (!pathExists(source)) {
        recordFailure(report, file, output,
                      "source file does not exist in the mod staging folder");
        return false;
    }

    if (pathExists(output) && platform::is_hardlink_to(source, output)) {
        ++report.skipped;
        journal.event("file.skip", fileDetails(file, output));
        return true;
    }

    try {
        ensureParent(output);

        const bool outputExists = pathExists(output);
        const bool backupExists = pathExists(backup);

        if (outputExists && backupExists) {
            if (platform::link_count(output) < 2) {
                recordFailure(
                    report, file, output,
                    "a .vortex_backup already exists but the target is not a hardlink; "
                    "remove the stale backup or resolve the file manually");
                return false;
            }
            fs::remove(output);
            journal.event("file.replace", fileDetails(file, output));
        } else if (outputExists) {
            fs::rename(output, backup);
            ++report.backedUp;
            journal.event("file.backup", fileDetails(file, output));
        }

        std::error_code ec;
        fs::create_hard_link(source, output, ec);
        if (ec == std::errc::file_exists) {
            fs::remove(output, ec);
            ec.clear();
            fs::create_hard_link(source, output, ec);
        }
        if (ec) {
            throw std::system_error(ec, "create hard link");
        }

        ++report.linked;
        journal.event("file.link", fileDetails(file, output));
        return true;
    } catch (const std::exception& err) {
        recordFailure(report, file, output, err.what());
        return false;
    }
}

} // namespace

ApplyReport applyDeployment(const DeploymentPlan& plan, const ExecutorContext& context,
                            Journal& journal) {
    ApplyReport report;
    journal.event("deploy.begin",
                  {
                      {"desired", plan.desired.size()},
                      {"added", plan.added.size()},
                      {"removed", plan.removed.size()},
                      {"changed", plan.changed.size()},
                  },
                  true);

    std::map<std::string, DeployedFile> finalEntries;
    for (const auto& entry : plan.desired) {
        finalEntries[file_key(entry, context.caseInsensitive)] = entry;
    }

    for (const auto& entry : plan.removed) {
        if (!removeEntry(context, entry, true, report, journal)) {
            finalEntries[file_key(entry, context.caseInsensitive)] = entry;
        }
    }

    for (const auto& change : plan.changed) {
        const auto key = file_key(change.desired, context.caseInsensitive);
        if (!removeEntry(context, change.previous, false, report, journal)) {
            finalEntries[key] = change.previous;
            continue;
        }
        if (!linkEntry(context, change.desired, report, journal)) {
            finalEntries.erase(key);
        }
    }

    for (const auto& entry : plan.added) {
        if (!linkEntry(context, entry, report, journal)) {
            finalEntries.erase(file_key(entry, context.caseInsensitive));
        }
    }

    Manifest manifest = makeEmptyManifest(context.previous, platform::now_ms());
    manifest.instance = context.instance;
    if (!context.deploymentMethod.empty()) {
        manifest.deploymentMethod = context.deploymentMethod;
    }
    if (!context.gameId.empty()) {
        manifest.gameId = context.gameId;
    }
    manifest.stagingPath = context.stagingPath.string();
    manifest.targetPath = context.targetPath.string();
    manifest.files.reserve(finalEntries.size());
    for (const auto& [key, entry] : finalEntries) {
        (void)key;
        manifest.files.push_back(entry);
    }

    saveManifestAtomic(context.manifestPath, manifest);
    journal.event("manifest.write", {{"path", context.manifestPath.string()}}, true);

    if (report.failures.empty()) {
        journal.event("deploy.complete", {{"linked", report.linked}}, true);
    } else {
        journal.event("deploy.failed",
                      {
                          {"linked", report.linked},
                          {"failures", report.failures.size()},
                      },
                      true);
    }
    return report;
}

ApplyReport purgeDeployment(const Manifest& manifest, const ExecutorContext& context,
                            Journal& journal) {
    ApplyReport report;
    journal.event("purge.begin", {{"files", manifest.files.size()}}, true);

    std::vector<DeployedFile> remaining;
    for (const auto& entry : manifest.files) {
        const auto failuresBefore = report.failures.size();
        removeEntry(context, entry, true, report, journal);
        if (report.failures.size() != failuresBefore) {
            // Keep failed entries so a later purge can retry them instead of
            // forgetting that they may still be deployed.
            remaining.push_back(entry);
        }
    }

    auto empty = makeEmptyManifest(manifest, platform::now_ms());
    empty.instance = context.instance.empty() ? manifest.instance : context.instance;
    empty.stagingPath = context.stagingPath.string();
    empty.targetPath = context.targetPath.string();
    empty.files = std::move(remaining);
    saveManifestAtomic(context.manifestPath, empty);
    journal.event("manifest.write", {{"path", context.manifestPath.string()}}, true);
    journal.event("purge.complete",
                  {{"removed", report.removed}, {"remaining", empty.files.size()}}, true);
    return report;
}

} // namespace vortex::deployment
