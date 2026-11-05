#pragma once

#include "vortex/deployment/types.hpp"

#include <filesystem>

namespace vortex::deployment {

Manifest loadManifest(const std::filesystem::path& path);
void saveManifestAtomic(const std::filesystem::path& path, const Manifest& manifest);
Manifest makeEmptyManifest(const Manifest& previous, std::int64_t deploymentTime);

} // namespace vortex::deployment
