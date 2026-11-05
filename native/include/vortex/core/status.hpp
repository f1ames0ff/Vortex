#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace vortex::core {

/**
 * State of a mod's staging folder, as the renderer needs it.
 *
 * This is deliberately a small, serialisable value: the renderer should not
 * have to walk the staging directory itself, and the core process can answer
 * this for thousands of mods without blocking the UI thread.
 */
enum class ModStatus {
    Ok,
    Empty,
    Missing,
    Error,
};

struct ModStatusQuery {
    std::string id;
    std::string installationPath;
};

struct ModStatusResult {
    std::string id;
    std::string installationPath;
    ModStatus status = ModStatus::Error;
    std::size_t entries = 0;
    std::string error;
};

const char* statusName(ModStatus status);

/**
 * Inspect one mod under the staging root. `installationPath` must be a
 * relative path inside `stagingPath`; absolute paths and `..` components are
 * rejected rather than followed.
 */
ModStatusResult inspectMod(const std::filesystem::path& stagingPath,
                           const ModStatusQuery& query);

/**
 * Inspect many mods in parallel. The result order matches `queries`.
 */
std::vector<ModStatusResult> inspectMods(const std::filesystem::path& stagingPath,
                                         const std::vector<ModStatusQuery>& queries);

} // namespace vortex::core
