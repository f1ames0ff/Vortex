#pragma once

#include "vortex/deployment/file_index.hpp"
#include "vortex/deployment/types.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace vortex::deployment {

std::string file_key(const DeployedFile& file, bool caseInsensitive);
std::string file_key(const std::optional<std::string>& target, const std::string& relPath,
                     bool caseInsensitive);

DeploymentPlan planDeployment(const std::filesystem::path& stagingPath,
                              const std::filesystem::path& targetPath,
                              const std::vector<ModSpec>& mods, const Manifest& previous,
                              const PlanOptions& options, IndexStore& indexStore);

/**
 * Plan the activation (or deactivation) of one mod while leaving every other
 * source in the previous manifest untouched. Used by the single-mod
 * activation path so it does not have to rescan/redeploy the whole profile.
 */
DeploymentPlan planSingleMod(const std::filesystem::path& stagingPath,
                             const std::filesystem::path& targetPath, const ModSpec& mod,
                             const Manifest& previous, const PlanOptions& options,
                             IndexStore& indexStore, bool enable);

} // namespace vortex::deployment
