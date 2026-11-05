#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace vortex::core {

/**
 * The instruction subset the generic installer emits. FOMOD script execution
 * stays in the existing .NET host; this module only applies the resulting
 * file operations.
 */
enum class InstallInstructionType {
    Mkdir,
    Copy,
    Hardlink,
    Write,
};

struct InstallInstruction {
    InstallInstructionType type = InstallInstructionType::Copy;
    // Relative to the extracted archive directory for copy/hardlink.
    std::string source;
    // Relative to the mod's staging destination.
    std::string destination;
    // Raw file content for write instructions.
    std::string data;
    // A missing optional source is skipped instead of reported as a failure.
    bool optional = false;
};

struct InstallRequest {
    std::filesystem::path tempPath;
    std::filesystem::path destinationPath;
    std::vector<InstallInstruction> instructions;
    // Optional JSONL journal path. Empty disables journalling.
    std::filesystem::path journalPath;
    // Optional caller-supplied operation id; generated when empty.
    std::string operationId;
    std::size_t concurrency = 8;
};

struct InstallFailure {
    std::string source;
    std::string destination;
    std::string message;
};

struct InstallReport {
    std::size_t directories = 0;
    std::size_t linked = 0;
    std::size_t copied = 0;
    std::size_t written = 0;
    std::vector<InstallFailure> failures;
};

/**
 * Apply installer instructions, writing into `destinationPath`.
 *
 * Copy/hardlink sources are resolved inside `tempPath`; destinations are
 * resolved inside `destinationPath`. Both reject absolute paths and `..`
 * components. Hardlink instructions fall back to a copy when the filesystem
 * does not support links (for example across devices).
 */
InstallReport applyInstall(const InstallRequest& request);

} // namespace vortex::core
