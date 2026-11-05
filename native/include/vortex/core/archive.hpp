#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace vortex::core {

struct ArchiveEntry {
    std::string path;
    std::uint64_t size = 0;
    bool directory = false;
    bool encrypted = false;
    bool symlink = false;
};

struct ArchiveListResult {
    int code = -1;
    std::vector<ArchiveEntry> entries;
    std::vector<std::string> errors;
    bool encrypted = false;
};

struct ArchiveExtractResult {
    int code = -1;
    std::vector<ArchiveEntry> entries;
    std::vector<std::string> errors;
};

/**
 * List an archive with the external 7-Zip binary. `sevenZip` is the path to
 * the existing binary; the core does not embed or reimplement 7-Zip.
 */
ArchiveListResult listArchive(const std::filesystem::path& archive,
                              const std::filesystem::path& sevenZip,
                              const std::string& password = std::string{});

/**
 * Validate every archive path, then extract into `destination`. Absolute and
 * `..` paths and symlink entries are rejected before extraction starts.
 */
ArchiveExtractResult extractArchive(const std::filesystem::path& archive,
                                    const std::filesystem::path& destination,
                                    const std::filesystem::path& sevenZip,
                                    const std::string& password = std::string{});

} // namespace vortex::core
