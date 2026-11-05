#pragma once

#include "vortex/deployment/types.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace vortex::deployment {

struct ExternalChange {
    std::string filePath;
    std::string source;
    std::optional<std::string> target;
    /** refchange, valchange, deleted or srcdeleted. */
    std::string changeType;
    std::optional<TimeMs> sourceTime;
    std::optional<TimeMs> destTime;
};

/**
 * Compare a deployment manifest with the files currently in staging and the
 * target directory. Detects deleted source files, deleted target files and
 * targets that no longer link to (or point at) their staging source.
 */
std::vector<ExternalChange> detectExternalChanges(
    const std::filesystem::path& stagingPath, const std::filesystem::path& targetPath,
    const std::string& deploymentMethod, const std::vector<DeployedFile>& files,
    bool caseInsensitive);

} // namespace vortex::deployment
