#include "vortex/game/descriptor.hpp"

#include "vortex/game/toml.hpp"

#include <algorithm>
#include <fstream>
#include <initializer_list>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <utility>

namespace fs = std::filesystem;

namespace vortex::game {

namespace {

const TomlValue* findAny(const TomlTable& table,
                         std::initializer_list<std::string_view> paths) {
    for (const auto path : paths) {
        if (const auto* value = findToml(table, path)) {
            return value;
        }
    }
    return nullptr;
}

[[noreturn]] void fieldError(const fs::path& source, const std::string& field,
                             const std::string& message) {
    throw std::runtime_error(source.string() + ": field '" + field + "' " + message);
}

std::string readString(const TomlTable& table, std::initializer_list<std::string_view> paths,
                       const std::string& field, const fs::path& source, bool required) {
    const auto* value = findAny(table, paths);
    if (value == nullptr) {
        if (required) {
            throw std::runtime_error(source.string() + ": missing required field '" + field +
                                     "'");
        }
        return {};
    }
    if (!value->isString()) {
        fieldError(source, field,
                   std::string("must be a string, got ") + tomlTypeName(value->type));
    }
    if (required && value->stringValue.empty()) {
        throw std::runtime_error(source.string() + ": missing required field '" + field +
                                 "'");
    }
    return value->stringValue;
}

std::optional<std::string> readOptionalString(const TomlTable& table,
                                              std::initializer_list<std::string_view> paths,
                                              const std::string& field,
                                              const fs::path& source) {
    auto value = readString(table, paths, field, source, false);
    if (value.empty()) {
        return std::nullopt;
    }
    return value;
}

std::vector<std::string> readStringList(const TomlTable& table,
                                        std::initializer_list<std::string_view> paths,
                                        const std::string& field,
                                        const fs::path& source) {
    const auto* value = findAny(table, paths);
    if (value == nullptr) {
        return {};
    }

    if (value->isString()) {
        return {value->stringValue};
    }
    if (!value->isArray()) {
        fieldError(source, field,
                   std::string("must be a string or an array of strings, got ") +
                       tomlTypeName(value->type));
    }

    std::vector<std::string> result;
    result.reserve(value->arrayValue.size());
    for (const auto& item : value->arrayValue) {
        if (!item.isString()) {
            fieldError(source, field,
                       std::string("must contain only strings, got ") +
                           tomlTypeName(item.type));
        }
        result.push_back(item.stringValue);
    }
    return result;
}

std::optional<std::int64_t> readSteamAppId(const TomlTable& table, const fs::path& source) {
    const auto* value =
        findAny(table, {"steamAppId", "game.steamAppId", "details.steamAppId"});
    if (value == nullptr) {
        return std::nullopt;
    }
    if (!value->isInteger()) {
        fieldError(source, "steamAppId",
                   std::string("must be an integer, got ") + tomlTypeName(value->type));
    }
    return value->integerValue;
}

} // namespace

GameDescriptor loadGameDescriptor(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("failed to open game descriptor: " + path.string());
    }
    const std::string text((std::istreambuf_iterator<char>(input)),
                           std::istreambuf_iterator<char>());

    const auto document = parseToml(text, path.string());

    GameDescriptor descriptor;
    descriptor.sourcePath = path;
    descriptor.id = readString(document, {"id", "game.id"}, "id", path, true);
    descriptor.name = readString(document, {"name", "game.name"}, "name", path, true);
    descriptor.nexusDomain =
        readString(document, {"nexusDomain", "game.nexusDomain", "details.nexusPageId"},
                   "nexusDomain", path, true);
    descriptor.steamAppId = readSteamAppId(document, path);
    descriptor.requiredFiles =
        readStringList(document, {"requiredFiles", "requirements.files"}, "requiredFiles",
                       path);
    descriptor.deploymentTargets =
        readStringList(document,
                       {"deploymentTargets", "deployment.targets", "deployment.paths",
                        "queryModPath"},
                       "deploymentTargets", path);
    descriptor.pluginPath = readOptionalString(
        document, {"pluginPath", "deployment.pluginPath"}, "pluginPath", path);
    descriptor.iniPath =
        readOptionalString(document, {"iniPath", "deployment.iniPath"}, "iniPath", path);
    descriptor.hooks =
        readStringList(document, {"hooks", "hooks.list", "hooks.enabled"}, "hooks", path);
    return descriptor;
}

std::vector<GameDescriptor> loadGameDescriptors(const fs::path& directory) {
    std::error_code ec;
    const auto status = fs::status(directory, ec);
    if (ec || !fs::exists(status)) {
        throw std::runtime_error("game descriptor directory does not exist: " +
                                 directory.string());
    }
    if (!fs::is_directory(status)) {
        throw std::runtime_error("game descriptor path is not a directory: " +
                                 directory.string());
    }

    std::vector<fs::path> descriptors;
    fs::directory_iterator iterator(directory, fs::directory_options::skip_permission_denied,
                                    ec);
    if (ec) {
        throw std::runtime_error("failed to scan game descriptor directory " +
                                 directory.string() + ": " + ec.message());
    }
    for (const auto& entry : iterator) {
        std::error_code entryError;
        if (!entry.is_directory(entryError) || entryError) {
            continue;
        }
        const auto candidate = entry.path() / "game.toml";
        std::error_code fileError;
        if (fs::is_regular_file(candidate, fileError) && !fileError) {
            descriptors.push_back(candidate);
        }
    }

    std::sort(descriptors.begin(), descriptors.end(),
              [](const fs::path& left, const fs::path& right) {
                  return left.parent_path().filename().generic_string() <
                         right.parent_path().filename().generic_string();
              });

    std::vector<GameDescriptor> result;
    result.reserve(descriptors.size());
    for (const auto& path : descriptors) {
        result.push_back(loadGameDescriptor(path));
    }
    return result;
}

} // namespace vortex::game
