#pragma once

#include <atomic>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace vortex::core {

struct ProcessResult {
    int exitCode = -1;
    std::string output;
    bool cancelled = false;
};

/**
 * Run a child process without a shell, capturing stdout and stderr into one
 * string. argv[0] is set to the executable path. When `cancel` becomes true
 * the child is terminated and `cancelled` is set; this is how download
 * pause/cancel interrupts an in-flight transfer.
 */
ProcessResult runProcess(const std::filesystem::path& executable,
                         const std::vector<std::string>& arguments,
                         const std::atomic<bool>* cancel = nullptr,
                         const std::map<std::string, std::string>& environment = {});

} // namespace vortex::core
