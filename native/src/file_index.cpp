#include "vortex/deployment/file_index.hpp"

#include "vortex/deployment/platform.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <set>
#include <fstream>
#include <stdexcept>
#include <system_error>

namespace fs = std::filesystem;
using nlohmann::json;

namespace vortex::deployment {

namespace {

constexpr const char* INDEX_FILE = ".vortex-file-index.json";
constexpr int INDEX_VERSION = 1;

TimeMs readTime(const json& value, const char* key) {
    if (!value.contains(key)) {
        return 0;
    }
    if (value[key].is_number_integer()) {
        return value[key].get<TimeMs>();
    }
    return static_cast<TimeMs>(value[key].get<double>());
}

} // namespace

IndexStore::IndexStore(fs::path stagingPath)
    : stagingPath_(std::move(stagingPath)), indexPath_(stagingPath_ / INDEX_FILE) {
    load();
}

void IndexStore::load() {
    std::ifstream input(indexPath_, std::ios::binary);
    if (!input) {
        return;
    }

    json value;
    try {
        input >> value;
    } catch (const std::exception&) {
        return;
    }

    if (!value.is_object() || value.value("version", 0) != INDEX_VERSION ||
        !value.contains("entries") || !value["entries"].is_object()) {
        return;
    }

    for (const auto& [folder, raw] : value["entries"].items()) {
        if (!raw.is_object() || !raw.contains("files") || !raw["files"].is_array()) {
            continue;
        }
        ModIndex index;
        index.rootMtimeMs = readTime(raw, "rootMtimeMs");
        for (const auto& file : raw["files"]) {
            if (!file.is_object()) {
                continue;
            }
            IndexedFile entry;
            entry.relPath = file.value("relPath", std::string{});
            entry.time = readTime(file, "time");
            index.files.push_back(std::move(entry));
        }
        entries_[folder] = std::move(index);
    }
}

ModIndex IndexStore::scan(const fs::path& sourcePath, TimeMs rootMtimeMs) {
    ModIndex index;
    index.rootMtimeMs = rootMtimeMs;

    std::error_code ec;
    fs::recursive_directory_iterator iterator(
        sourcePath, fs::directory_options::skip_permission_denied, ec);
    if (ec) {
        throw std::system_error(ec, "scan mod directory");
    }

    for (const auto& entry : iterator) {
        if (entry.is_directory()) {
            continue;
        }
        if (entry.is_symlink()) {
            // Directory symlinks are not followed; a file symlink is still a file.
        }

        std::error_code relEc;
        const auto rel = fs::relative(entry.path(), sourcePath, relEc);
        if (relEc) {
            continue;
        }

        try {
            index.files.push_back({rel.generic_string(), platform::file_mtime_ms(entry.path())});
        } catch (const std::exception&) {
            // A file can disappear while the directory is being scanned.
        }
    }

    std::sort(index.files.begin(), index.files.end(),
              [](const IndexedFile& lhs, const IndexedFile& rhs) {
                  return lhs.relPath < rhs.relPath;
              });
    return index;
}

const ModIndex& IndexStore::get(const fs::path& sourcePath) {
    const auto rootMtime = platform::file_mtime_ms(sourcePath);
    const auto folder = sourcePath.filename().string();

    auto found = entries_.find(folder);
    if (found != entries_.end() && found->second.rootMtimeMs == rootMtime) {
        return found->second;
    }

    auto index = scan(sourcePath, rootMtime);
    auto [position, inserted] = entries_.insert_or_assign(folder, std::move(index));
    (void)inserted;
    dirty_ = true;
    return position->second;
}

void IndexStore::verifyFiles(const fs::path& sourcePath,
                            const std::vector<std::string>& relPaths) {
    const auto folder = sourcePath.filename().string();
    auto found = entries_.find(folder);
    if (found == entries_.end() || relPaths.empty()) {
        return;
    }

    const std::set<std::string> expected(relPaths.begin(), relPaths.end());
    std::vector<IndexedFile> kept;
    kept.reserve(found->second.files.size());
    bool changed = false;

    for (const auto& file : found->second.files) {
        if (!expected.contains(file.relPath)) {
            kept.push_back(file);
            continue;
        }

        try {
            const auto time = platform::file_mtime_ms(sourcePath / fs::path(file.relPath));
            if (time != file.time) {
                kept.push_back({file.relPath, time});
                changed = true;
            } else {
                kept.push_back(file);
            }
        } catch (const std::system_error& err) {
            if (err.code() == std::errc::no_such_file_or_directory) {
                changed = true;
                continue;
            }
            throw;
        }
    }

    if (changed) {
        found->second.files = std::move(kept);
        dirty_ = true;
    }
}

void IndexStore::flush() {
    if (!dirty_) {
        return;
    }

    json root = {
        {"version", INDEX_VERSION},
        {"entries", json::object()},
    };
    for (const auto& [folder, index] : entries_) {
        json files = json::array();
        for (const auto& file : index.files) {
            files.push_back({{"relPath", file.relPath}, {"time", file.time}});
        }
        root["entries"][folder] = {
            {"rootMtimeMs", index.rootMtimeMs},
            {"files", std::move(files)},
        };
    }

    const auto temp = fs::path(indexPath_.native() +
                               fs::path(".tmp-" + platform::operation_id()).native());
    {
        std::ofstream output(temp, std::ios::binary | std::ios::trunc);
        if (!output) {
            throw std::runtime_error("failed to create temporary index: " + temp.string());
        }
        output << root.dump();
        output.flush();
        if (!output) {
            throw std::runtime_error("failed to write temporary index: " + temp.string());
        }
    }

    try {
        platform::sync_file(temp);
        platform::atomic_replace(temp, indexPath_);
        dirty_ = false;
    } catch (...) {
        std::error_code ec;
        fs::remove(temp, ec);
        throw;
    }
}

} // namespace vortex::deployment
