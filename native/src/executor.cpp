#include "vortex/deployment/executor.hpp"

#include "vortex/deployment/manifest.hpp"
#include "vortex/deployment/platform.hpp"
#include "vortex/deployment/planner.hpp"

#include <fstream>
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

constexpr const char* MOVE_LINK_EXT = ".vortex_lnk";

fs::path moveLinkPath(const fs::path& source) {
    return fs::path(source.native() + fs::path(MOVE_LINK_EXT).native());
}

std::optional<fs::path> readMoveTarget(const fs::path& source) {
    std::ifstream input(moveLinkPath(source), std::ios::binary);
    if (!input) {
        return std::nullopt;
    }
    try {
        nlohmann::json value;
        input >> value;
        if (!value.is_object() || !value.contains("target") || !value["target"].is_string()) {
            return std::nullopt;
        }
        return fs::path(value["target"].get<std::string>());
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

bool samePath(const fs::path& lhs, const fs::path& rhs, bool caseInsensitive) {
    return platform::normalized_key(lhs, caseInsensitive) ==
           platform::normalized_key(rhs, caseInsensitive);
}

bool isOwnDeployment(const ExecutorContext& context, const fs::path& source,
                     const fs::path& output) {
    const auto method = platform::normalize_deployment_method(context.deploymentMethod);
    if (method == "symlink") {
        return platform::is_symlink_to(source, output);
    }
    if (method == "move") {
        const auto target = readMoveTarget(source);
        return target.has_value() && samePath(*target, output, context.caseInsensitive);
    }
    return platform::is_hardlink_to(source, output);
}

std::string deploymentMethod(const ExecutorContext& context) {
    return platform::normalize_deployment_method(
        context.deploymentMethod.empty() ? std::string("hardlink") : context.deploymentMethod);
}

void relocate(const fs::path& from, const fs::path& to) {
    std::error_code ec;
    fs::rename(from, to, ec);
    if (!ec) {
        return;
    }
    if (ec != std::errc::cross_device_link) {
        throw std::system_error(ec, "move file");
    }
    fs::copy_file(from, to, fs::copy_options::overwrite_existing, ec);
    if (ec) {
        throw std::system_error(ec, "copy across devices");
    }
    fs::remove(from, ec);
    if (ec) {
        throw std::system_error(ec, "remove moved source");
    }
}

void writeMoveLink(const fs::path& source, const fs::path& output) {
    std::error_code ec;
    fs::create_directories(source.parent_path(), ec);
    if (ec) {
        throw std::system_error(ec, "create move link directory");
    }
    std::ofstream link(moveLinkPath(source), std::ios::binary | std::ios::trunc);
    if (!link) {
        throw std::runtime_error("failed to create move link: " + moveLinkPath(source).string());
    }
    link << nlohmann::json{{"target", output.string()}}.dump();
    link.flush();
    if (!link) {
        throw std::runtime_error("failed to write move link: " + moveLinkPath(source).string());
    }

    std::error_code timeEc;
    const auto sourceTime = fs::last_write_time(source, timeEc);
    if (!timeEc) {
        fs::last_write_time(moveLinkPath(source), sourceTime, timeEc);
    }
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

bool removeMoveEntry(const ExecutorContext& context, const DeployedFile& file,
                     bool restoreBackup, ApplyReport& report, Journal& journal) {
    const auto output = outputPath(context, file);
    const auto source = sourcePath(context, file);
    const auto backup = platform::backup_path(output);
    const auto link = moveLinkPath(source);

    const auto target = readMoveTarget(source);
    if (!target.has_value()) {
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
        recordFailure(report, file, output,
                      "the deployment has no .vortex_lnk and the target exists; refusing to "
                      "remove a file Vortex does not own");
        return false;
    }

    if (!samePath(*target, output, context.caseInsensitive)) {
        recordFailure(report, file, output,
                      "the .vortex_lnk points at a different file; refusing to move it");
        return false;
    }

    try {
        if (pathExists(output)) {
            ensureParent(source);
            relocate(output, source);
            ++report.removed;
            journal.event("file.unlink", fileDetails(file, output));
            platform::remove_empty_parents(output.parent_path(), context.targetPath);
        }
        std::error_code ec;
        fs::remove(link, ec);
        if (ec && ec != std::errc::no_such_file_or_directory) {
            throw std::system_error(ec, "remove move link");
        }

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

bool removeEntry(const ExecutorContext& context, const DeployedFile& file, bool restoreBackup,
                 ApplyReport& report, Journal& journal) {
    const auto output = outputPath(context, file);
    const auto source = sourcePath(context, file);
    const auto backup = platform::backup_path(output);

    if (deploymentMethod(context) == "move") {
        return removeMoveEntry(context, file, restoreBackup, report, journal);
    }

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

    if (!isOwnDeployment(context, source, output)) {
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

bool linkMoveEntry(const ExecutorContext& context, const DeployedFile& file,
                   ApplyReport& report, Journal& journal) {
    const auto source = sourcePath(context, file);
    const auto output = outputPath(context, file);
    const auto backup = platform::backup_path(output);

    if (pathExists(output) && isOwnDeployment(context, source, output)) {
        ++report.skipped;
        journal.event("file.skip", fileDetails(file, output));
        return true;
    }

    if (!pathExists(source)) {
        recordFailure(report, file, output,
                      "source file does not exist in the mod staging folder");
        return false;
    }

    try {
        ensureParent(output);

        if (pathExists(output)) {
            if (pathExists(backup)) {
                recordFailure(
                    report, file, output,
                    "a .vortex_backup already exists but the target is not owned by this "
                    "deployment; remove the stale backup or resolve the file manually");
                return false;
            }
            fs::rename(output, backup);
            ++report.backedUp;
            journal.event("file.backup", fileDetails(file, output));
        }

        writeMoveLink(source, output);
        relocate(source, output);

        ++report.linked;
        journal.event("file.move", fileDetails(file, output));
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

    const auto method = deploymentMethod(context);
    if (method == "move") {
        return linkMoveEntry(context, file, report, journal);
    }
    if (method != "hardlink" && method != "symlink") {
        recordFailure(report, file, output, "unsupported deployment method: " + method);
        return false;
    }

    if (!pathExists(source)) {
        recordFailure(report, file, output,
                      "source file does not exist in the mod staging folder");
        return false;
    }

    if (pathExists(output) && isOwnDeployment(context, source, output)) {
        ++report.skipped;
        journal.event("file.skip", fileDetails(file, output));
        return true;
    }

    try {
        ensureParent(output);

        const bool outputExists = pathExists(output);
        const bool backupExists = pathExists(backup);

        if (outputExists && backupExists) {
            if (!isOwnDeployment(context, source, output)) {
                recordFailure(
                    report, file, output,
                    "a .vortex_backup already exists but the target is not owned by this "
                    "deployment; remove the stale backup or resolve the file manually");
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
        if (method == "symlink") {
            fs::create_symlink(source, output, ec);
            if (ec == std::errc::file_exists) {
                fs::remove(output, ec);
                ec.clear();
                fs::create_symlink(source, output, ec);
            }
            if (ec) {
                throw std::system_error(ec, "create symlink");
            }
        } else {
            fs::create_hard_link(source, output, ec);
            if (ec == std::errc::file_exists) {
                fs::remove(output, ec);
                ec.clear();
                fs::create_hard_link(source, output, ec);
            }
            if (ec) {
                throw std::system_error(ec, "create hard link");
            }
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
