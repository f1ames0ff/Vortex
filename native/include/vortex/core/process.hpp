#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace vortex::core {

struct ProcessResult {
    int exitCode = -1;
    std::string output;
};

/**
 * Run a child process without a shell, capturing stdout and stderr into one
 * string. argv[0] is set to the executable path.
 */
ProcessResult runProcess(const std::filesystem::path& executable,
                         const std::vector<std::string>& arguments);

} // namespace vortex::core
