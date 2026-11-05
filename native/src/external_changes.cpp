#include "vortex/deployment/external_changes.hpp"

#include "vortex/deployment/platform.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <map>
#include <system_error>

namespace fs = std::filesystem;

namespace vortex::deployment {

namespace {

constexpr const char* MOVE_LINK_EXT = ".vortex_lnk";

fs::path sourcePath(const fs::path& stagingPath, const DeployedFile& file) {
    return stagingPath / fs::path(file.source) / fs::path(file.relPath);
}

fs::path outputPath(const fs::path& targetPath, const DeployedFile& file) {
    fs::path result = targetPath;
    if (file.target.has_value() && !file.target->empty()) {
        result /= fs::path(*file.target);
    }
    return result / fs::path(file.relPath);
}

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

std::optional<TimeMs> mtime(const fs::path& path) {
    std::error_code ec;
    const auto status = fs::status(path, ec);
    if (ec || !fs::exists(status)) {
        return std::nullopt;
    }
    try {
        return platform::file_mtime_ms(path);
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

bool samePath(const fs::path& lhs, const fs::path& rhs, bool caseInsensitive) {
    return platform::normalized_key(lhs, caseInsensitive) ==
           platform::normalized_key(rhs, caseInsensitive);
}

bool isOwnDeployment(const std::string& method, const fs::path& source, const fs::path& output,
                     bool caseInsensitive) {
    if (method == "symlink") {
        return platform::is_symlink_to(source, output);
    }
    if (method == "move") {
        const auto target = readMoveTarget(source);
        return target.has_value() && samePath(*target, output, caseInsensitive);
    }
    return platform::is_hardlink_to(source, output);
}

} // namespace

std::vector<ExternalChange> detectExternalChanges(const fs::path& stagingPath,
                                                  const fs::path& targetPath,
                                                  const std::string& deploymentMethod,
                                                  const std::vector<DeployedFile>& files,
                                                  bool caseInsensitive) {
    const auto method = platform::normalize_deployment_method(deploymentMethod);
    const bool canRestore = method != "symlink";

    std::map<std::string, ExternalChange> deduplicated;
    for (const auto& file : files) {
        const auto source = sourcePath(stagingPath, file);
        const auto output = outputPath(targetPath, file);

        bool sourceMissing = false;
        std::optional<TimeMs> sourceTime;
        const auto sourceStatus = mtime(source);
        if (sourceStatus.has_value()) {
            sourceTime = sourceStatus;
        } else if (method == "move") {
            // A moved file no longer exists in staging; its placeholder points
            // at the deployed copy, which is the source of truth while it is
            // deployed.
            const auto target = readMoveTarget(source);
            if (target.has_value()) {
                sourceTime = mtime(*target);
            }
            sourceMissing = !sourceTime.has_value();
        } else {
            sourceMissing = true;
        }

        const auto destTime = mtime(output);
        const bool destMissing = !destTime.has_value();

        ExternalChange change;
        change.filePath = file.relPath;
        change.source = file.source;
        change.target = file.target;

        if (sourceMissing && !destMissing) {
            if (!canRestore) {
                continue;
            }
            change.changeType = "srcdeleted";
        } else if (destMissing && !sourceMissing) {
            change.changeType = "deleted";
        } else if (!sourceMissing && !destMissing &&
                   !isOwnDeployment(method, source, output, caseInsensitive)) {
            change.changeType = "refchange";
            change.sourceTime = sourceTime;
            change.destTime = destTime;
        } else {
            continue;
        }

        // A deleted source/target is the more important observation when a
        // manifest contains the same deployed key twice.
        const auto key = file.source + "/" + file.relPath;
        const auto found = deduplicated.find(key);
        if (found == deduplicated.end() ||
            !(found->second.changeType == "deleted" ||
              found->second.changeType == "srcdeleted")) {
            deduplicated[key] = std::move(change);
        }
    }

    std::vector<ExternalChange> result;
    result.reserve(deduplicated.size());
    for (auto& entry : deduplicated) {
        result.push_back(std::move(entry.second));
    }
    return result;
}

} // namespace vortex::deployment
