#include "vortex/deployment/manifest.hpp"

#include "vortex/deployment/platform.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <stdexcept>
#include <string>
#include <system_error>

namespace fs = std::filesystem;
using nlohmann::json;

namespace vortex::deployment {

namespace {

TimeMs readTime(const json& value, const char* key, TimeMs fallback = 0) {
    if (!value.contains(key)) {
        return fallback;
    }
    if (value[key].is_number_integer()) {
        return value[key].get<TimeMs>();
    }
    if (value[key].is_number_float()) {
        return static_cast<TimeMs>(value[key].get<double>());
    }
    return fallback;
}

DeployedFile readFile(const json& value) {
    DeployedFile file;
    file.relPath = value.value("relPath", std::string{});
    file.source = value.value("source", std::string{});
    if (value.contains("target") && !value["target"].is_null()) {
        file.target = value["target"].get<std::string>();
    }
    file.time = readTime(value, "time");
    if (value.contains("merged") && value["merged"].is_array()) {
        file.merged = value["merged"].get<std::vector<std::string>>();
    }
    return file;
}

json writeFile(const DeployedFile& file) {
    json value = {
        {"relPath", file.relPath},
        {"source", file.source},
        {"time", file.time},
    };
    if (file.target.has_value()) {
        value["target"] = *file.target;
    }
    if (!file.merged.empty()) {
        value["merged"] = file.merged;
    }
    return value;
}

} // namespace

Manifest loadManifest(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("failed to open manifest: " + path.string());
    }

    json value;
    try {
        input >> value;
    } catch (const std::exception& err) {
        throw std::runtime_error("failed to parse manifest " + path.string() + ": " + err.what());
    }

    if (!value.is_object()) {
        throw std::runtime_error("manifest is not a JSON object: " + path.string());
    }

    Manifest manifest;
    manifest.version = value.value("version", 1);
    manifest.instance = value.value("instance", std::string{});
    if (value.contains("deploymentMethod") && !value["deploymentMethod"].is_null()) {
        manifest.deploymentMethod = value["deploymentMethod"].get<std::string>();
    }
    if (value.contains("gameId") && !value["gameId"].is_null()) {
        manifest.gameId = value["gameId"].get<std::string>();
    }
    manifest.stagingPath = value.value("stagingPath", std::string{});
    if (value.contains("targetPath") && !value["targetPath"].is_null()) {
        manifest.targetPath = value["targetPath"].get<std::string>();
    }
    manifest.deploymentTime = readTime(value, "deploymentTime");

    if (value.contains("files")) {
        if (!value["files"].is_array()) {
            throw std::runtime_error("manifest files is not an array: " + path.string());
        }
        manifest.files.reserve(value["files"].size());
        for (const auto& entry : value["files"]) {
            manifest.files.push_back(readFile(entry));
        }
    }

    return manifest;
}

void saveManifestAtomic(const fs::path& path, const Manifest& manifest) {
    json value = {
        {"instance", manifest.instance},
        {"version", manifest.version},
        {"gameId", manifest.gameId.value_or("")},
        {"deploymentTime", manifest.deploymentTime},
        {"stagingPath", manifest.stagingPath},
        {"files", json::array()},
    };
    if (manifest.deploymentMethod.has_value()) {
        value["deploymentMethod"] = *manifest.deploymentMethod;
    } else {
        value["deploymentMethod"] = nullptr;
    }
    if (manifest.targetPath.has_value()) {
        value["targetPath"] = *manifest.targetPath;
    }
    for (const auto& file : manifest.files) {
        value["files"].push_back(writeFile(file));
    }

    std::error_code ec;
    const auto parent = path.parent_path();
    if (!parent.empty()) {
        fs::create_directories(parent, ec);
        if (ec) {
            throw std::system_error(ec, "create manifest directory");
        }
    }

    const auto temp = fs::path(path.native() + fs::path(".tmp-" + platform::operation_id()).native());
    {
        std::ofstream output(temp, std::ios::binary | std::ios::trunc);
        if (!output) {
            throw std::runtime_error("failed to create temporary manifest: " + temp.string());
        }
        output << value.dump(2);
        output.flush();
        if (!output) {
            throw std::runtime_error("failed to write temporary manifest: " + temp.string());
        }
    }

    try {
        platform::sync_file(temp);
        platform::atomic_replace(temp, path);
    } catch (...) {
        std::error_code cleanup;
        fs::remove(temp, cleanup);
        throw;
    }
}

Manifest makeEmptyManifest(const Manifest& previous, std::int64_t deploymentTime) {
    Manifest result;
    result.version = previous.version;
    result.instance = previous.instance;
    result.deploymentMethod = previous.deploymentMethod;
    result.gameId = previous.gameId;
    result.stagingPath = previous.stagingPath;
    result.targetPath = previous.targetPath;
    result.deploymentTime = deploymentTime;
    return result;
}

} // namespace vortex::deployment
