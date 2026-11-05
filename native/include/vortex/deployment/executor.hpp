#pragma once

#include "vortex/deployment/journal.hpp"
#include "vortex/deployment/types.hpp"

#include <filesystem>
#include <string>

namespace vortex::deployment {

struct ExecutorContext {
    std::filesystem::path stagingPath;
    std::filesystem::path targetPath;
    std::filesystem::path manifestPath;
    Manifest previous;
    std::string instance;
    std::string deploymentMethod;
    std::string gameId;
    bool caseInsensitive = false;
};

ApplyReport applyDeployment(const DeploymentPlan& plan, const ExecutorContext& context,
                            Journal& journal);

ApplyReport purgeDeployment(const Manifest& manifest, const ExecutorContext& context,
                            Journal& journal);

} // namespace vortex::deployment
