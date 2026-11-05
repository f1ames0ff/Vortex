#include "vortex/core/status.hpp"

#include <algorithm>
#include <atomic>
#include <thread>
#include <system_error>

namespace fs = std::filesystem;

namespace vortex::core {

namespace {

bool isSafeRelative(const fs::path& path) {
    if (path.empty() || path.is_absolute() || path.has_root_name() ||
        path.has_root_directory()) {
        return false;
    }
    for (const auto& part : path) {
        if (part == "..") {
            return false;
        }
    }
    return true;
}

ModStatusResult errorResult(ModStatusResult result, const std::string& message) {
    result.status = ModStatus::Error;
    result.error = message;
    return result;
}

} // namespace

const char* statusName(ModStatus status) {
    switch (status) {
    case ModStatus::Ok:
        return "ok";
    case ModStatus::Empty:
        return "empty";
    case ModStatus::Missing:
        return "missing";
    case ModStatus::Error:
        return "error";
    }
    return "error";
}

ModStatusResult inspectMod(const fs::path& stagingPath, const ModStatusQuery& query) {
    ModStatusResult result;
    result.id = query.id;
    result.installationPath = query.installationPath;

    const fs::path relative(query.installationPath);
    if (!isSafeRelative(relative)) {
        return errorResult(std::move(result),
                           "installationPath must be a relative path inside staging");
    }

    const auto modPath = stagingPath / relative;
    std::error_code ec;
    const auto status = fs::status(modPath, ec);
    if (ec) {
        if (ec == std::errc::no_such_file_or_directory || ec == std::errc::not_a_directory) {
            result.status = ModStatus::Missing;
            return result;
        }
        return errorResult(std::move(result), ec.message());
    }
    if (!fs::exists(status)) {
        result.status = ModStatus::Missing;
        return result;
    }
    if (!fs::is_directory(status)) {
        return errorResult(std::move(result), "installationPath is not a directory");
    }

    std::size_t entries = 0;
    fs::directory_iterator iterator(modPath, fs::directory_options::skip_permission_denied, ec);
    if (ec) {
        return errorResult(std::move(result), ec.message());
    }
    for (const auto& entry : iterator) {
        (void)entry;
        ++entries;
    }

    result.entries = entries;
    result.status = entries == 0 ? ModStatus::Empty : ModStatus::Ok;
    return result;
}

std::vector<ModStatusResult> inspectMods(const fs::path& stagingPath,
                                         const std::vector<ModStatusQuery>& queries) {
    std::vector<ModStatusResult> results(queries.size());
    if (queries.empty()) {
        return results;
    }

    const auto hardware = std::thread::hardware_concurrency();
    const auto workers = std::clamp<std::size_t>(hardware == 0 ? 1 : hardware, 1, 8);
    const auto workerCount = std::min<std::size_t>(workers, queries.size());
    std::atomic<std::size_t> next{0};

    std::vector<std::thread> threads;
    threads.reserve(workerCount);
    for (std::size_t worker = 0; worker < workerCount; ++worker) {
        threads.emplace_back([&] {
            for (;;) {
                const auto index = next.fetch_add(1, std::memory_order_relaxed);
                if (index >= queries.size()) {
                    return;
                }
                results[index] = inspectMod(stagingPath, queries[index]);
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }

    return results;
}

} // namespace vortex::core
