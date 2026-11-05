#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace vortex::core {

enum class HashAlgorithm {
    Md5,
    Xxh64,
};

struct HashResult {
    std::string path;
    std::string hash;
    std::uint64_t bytes = 0;
};

struct HashFailure {
    std::string path;
    std::string message;
};

struct HashFilesResult {
    std::vector<HashResult> results;
    std::vector<HashFailure> failures;
};

HashAlgorithm parseHashAlgorithm(const std::string& value);
const char* hashAlgorithmName(HashAlgorithm algorithm);

/**
 * Hash files on worker threads. Results keep the input order; unreadable
 * files are reported in `failures` without aborting the batch.
 */
HashFilesResult hashFiles(const std::vector<std::string>& paths, HashAlgorithm algorithm,
                          std::size_t concurrency = 8);

} // namespace vortex::core
