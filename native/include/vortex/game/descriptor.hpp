#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace vortex::game {

/**
 * First-party game description loaded from a game.toml file.
 *
 * The schema mirrors the vocabulary of the TypeScript game extensions where it
 * makes sense: `id`/`name` identify the game, `nexusDomain` is the Nexus Mods
 * page slug (`details.nexusPageId`), `steamAppId` is
 * `details.steamAppId`, `requiredFiles` is the executable requirement list and
 * `deploymentTargets` is the resolved `queryModPath`. `pluginPath`, `iniPath`
 * and `hooks` carry the optional gamebryo-style extras the core needs later.
 */
struct GameDescriptor {
    std::string id;
    std::string name;
    std::string nexusDomain;
    std::optional<std::int64_t> steamAppId;
    std::vector<std::string> requiredFiles;
    std::vector<std::string> deploymentTargets;
    std::optional<std::string> pluginPath;
    std::optional<std::string> iniPath;
    std::vector<std::string> hooks;
    std::filesystem::path sourcePath;
};

/**
 * Load one descriptor from a game.toml file.
 *
 * Throws std::runtime_error when the file cannot be opened or a required field
 * is missing, and TomlError (a std::runtime_error) for malformed TOML.
 */
GameDescriptor loadGameDescriptor(const std::filesystem::path& path);

/**
 * Scan the immediate child directories of `directory` for game.toml files.
 * The result is sorted by child directory name so callers get a stable order.
 */
std::vector<GameDescriptor> loadGameDescriptors(const std::filesystem::path& directory);

} // namespace vortex::game
