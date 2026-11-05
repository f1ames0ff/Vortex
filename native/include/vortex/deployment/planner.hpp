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

} // namespace vortex::deployment
