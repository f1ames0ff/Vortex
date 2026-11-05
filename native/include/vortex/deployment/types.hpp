#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace vortex::deployment {

using TimeMs = std::int64_t;

struct DeployedFile {
    std::string relPath;
    std::string source;
    std::optional<std::string> target;
    TimeMs time = 0;
    std::vector<std::string> merged;
};

struct Manifest {
    int version = 1;
    std::string instance;
    std::optional<std::string> deploymentMethod;
    std::optional<std::string> gameId;
    std::string stagingPath;
    std::optional<std::string> targetPath;
    TimeMs deploymentTime = 0;
    std::vector<DeployedFile> files;
};

struct IndexedFile {
    std::string relPath;
    TimeMs time = 0;
};

struct ModIndex {
    TimeMs rootMtimeMs = 0;
    std::vector<IndexedFile> files;
};

struct ModSpec {
    std::string installationPath;
    std::string sourceName;
    std::string target;
    std::vector<std::string> fileOverrides;
};

struct PlanOptions {
    bool caseInsensitive = false;
    std::vector<std::string> extraBlacklist;
    std::string deploymentMethod = "hardlink";
};

struct ChangedFile {
    DeployedFile previous;
    DeployedFile desired;
};

struct DeploymentPlan {
    std::vector<DeployedFile> desired;
    std::vector<DeployedFile> added;
    std::vector<DeployedFile> removed;
    std::vector<ChangedFile> changed;
};

struct OperationFailure {
    std::string source;
    std::string relPath;
    std::string outputPath;
    std::string message;
};

struct ApplyReport {
    std::size_t linked = 0;
    std::size_t removed = 0;
    std::size_t backedUp = 0;
    std::size_t restored = 0;
    std::size_t skipped = 0;
    std::vector<OperationFailure> failures;
};

} // namespace vortex::deployment
