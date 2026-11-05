#include "vortex/deployment/planner.hpp"

#include "vortex/deployment/platform.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <system_error>

namespace fs = std::filesystem;

namespace vortex::deployment {

namespace {

constexpr const char* OVERRIDE_FILE = "vortex_override_instructions.json";
constexpr const char* MOVE_LINK_EXT = ".vortex_lnk";

std::string lowerAscii(std::string value) {
    for (char& ch : value) {
        if (ch >= 'A' && ch <= 'Z') {
            ch = static_cast<char>(ch - 'A' + 'a');
        }
    }
    return value;
}

bool isBlacklisted(const std::string& sourceRelativePath,
                   const std::vector<std::string>& extraBlacklist, bool caseInsensitive) {
    const fs::path path(sourceRelativePath);
    const auto normalized = platform::normalized_key(path, caseInsensitive);

    for (const auto& entry : extraBlacklist) {
        if (platform::normalized_key(fs::path(entry), caseInsensitive) == normalized) {
            return true;
        }
    }

    for (const auto& component : path) {
        const auto name = lowerAscii(component.string());
        if (name == ".git" || name == "_macosx" || name == "__macosx") {
            return true;
        }
    }

    const auto name = lowerAscii(path.filename().string());
    if (name.size() >= std::string(MOVE_LINK_EXT).size() &&
        name.compare(name.size() - std::string(MOVE_LINK_EXT).size(),
                     std::string(MOVE_LINK_EXT).size(), MOVE_LINK_EXT) == 0) {
        return true;
    }
    return name == ".gitignore" || name == ".hgignore" || name == ".gitattributes" ||
           name == "meta.ini" || name == lowerAscii(OVERRIDE_FILE);
}

bool hasMoveSuffix(const std::string& relPath) {
    const auto suffix = std::string(MOVE_LINK_EXT);
    return relPath.size() > suffix.size() &&
           relPath.compare(relPath.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::optional<fs::path> readMoveTarget(const fs::path& source) {
        std::ifstream input(
        fs::path(source.native() + fs::path(MOVE_LINK_EXT).native()), std::ios::binary);
    if (!input) {
        return std::nullopt;
    }
    try {
        nlohmann::json value;
        input >> value;
        if (!value.is_object() || !value.contains("target") || !value["target"].is_string()) {
            return std::nullopt;
        }
        return fs::path(value["target"].get<std::string>());
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

std::set<std::string> overrideKeys(const ModSpec& mod, const fs::path& targetPath,
                                   bool caseInsensitive) {
    std::set<std::string> result;
    for (const auto& overridePath : mod.fileOverrides) {
        fs::path value(overridePath);
        if (value.is_absolute()) {
            std::error_code ec;
            value = fs::relative(value, targetPath, ec);
            if (ec) {
                continue;
            }
        }
        result.insert(platform::normalized_key(value, caseInsensitive));
    }
    return result;
}

} // namespace

std::string file_key(const std::optional<std::string>& target, const std::string& relPath,
                     bool caseInsensitive) {
    fs::path path;
    if (target.has_value() && !target->empty()) {
        path = fs::path(*target) / fs::path(relPath);
    } else {
        path = fs::path(relPath);
    }
    return platform::normalized_key(path, caseInsensitive);
}

std::string file_key(const DeployedFile& file, bool caseInsensitive) {
    return file_key(file.target, file.relPath, caseInsensitive);
}

DeploymentPlan planDeployment(const fs::path& stagingPath, const fs::path& targetPath,
                              const std::vector<ModSpec>& mods, const Manifest& previous,
                              const PlanOptions& options, IndexStore& indexStore) {
    std::map<std::string, DeployedFile> previousByKey;
    std::map<std::string, std::vector<std::string>> previousPathsBySource;
    for (const auto& file : previous.files) {
        previousByKey[file_key(file, options.caseInsensitive)] = file;
        previousPathsBySource[file.source].push_back(file.relPath);
    }

    const bool move =
        platform::normalize_deployment_method(options.deploymentMethod) == "move";

    std::map<std::string, DeployedFile> desired;
    for (const auto& mod : mods) {
        const auto sourceRoot = stagingPath / fs::path(mod.installationPath);
        const auto sourceName = mod.sourceName.empty() ? mod.installationPath : mod.sourceName;

        const ModIndex* indexPtr = nullptr;
        try {
            indexPtr = &indexStore.get(sourceRoot);
        } catch (const std::system_error& err) {
            if (err.code() == std::errc::no_such_file_or_directory) {
                continue;
            }
            throw;
        }

        auto previousPaths = previousPathsBySource.find(sourceName);
        if (previousPaths == previousPathsBySource.end()) {
            previousPaths = previousPathsBySource.find(mod.installationPath);
        }
        if (previousPaths != previousPathsBySource.end()) {
            indexStore.verifyFiles(sourceRoot, previousPaths->second);
        }

        const auto& index = *indexPtr;
        const auto overrides = overrideKeys(mod, targetPath, options.caseInsensitive);

        for (const auto& file : index.files) {
            const auto sourceRelative = fs::path(mod.installationPath) / fs::path(file.relPath);
            if (isBlacklisted(sourceRelative.generic_string(), options.extraBlacklist,
                              options.caseInsensitive)) {
                if (!move || !hasMoveSuffix(file.relPath)) {
                    continue;
                }
            }

            DeployedFile entry;
            if (hasMoveSuffix(file.relPath)) {
                if (!move) {
                    continue;
                }
                entry.relPath =
                    file.relPath.substr(0, file.relPath.size() - std::string(MOVE_LINK_EXT).size());
                const auto target = readMoveTarget(sourceRoot / fs::path(entry.relPath));
                if (!target.has_value()) {
                    continue;
                }
                fs::path expected = targetPath;
                if (!mod.target.empty()) {
                    expected /= fs::path(mod.target);
                }
                expected /= fs::path(entry.relPath);
                if (platform::normalized_key(*target, options.caseInsensitive) !=
                    platform::normalized_key(expected, options.caseInsensitive)) {
                    continue;
                }
            } else {
                entry.relPath = file.relPath;
            }
            entry.source = sourceName;
            if (!mod.target.empty()) {
                entry.target = mod.target;
            }
            entry.time = file.time;

            const auto key = file_key(entry, options.caseInsensitive);
            if (overrides.contains(key)) {
                continue;
            }

            auto previousEntry = previousByKey.find(key);
            if (previousEntry != previousByKey.end() && !previousEntry->second.merged.empty()) {
                entry.merged = previousEntry->second.merged;
            }
            desired[key] = std::move(entry);
        }
    }

    DeploymentPlan plan;
    plan.desired.reserve(desired.size());

    for (const auto& [key, entry] : desired) {
        plan.desired.push_back(entry);
        auto previousEntry = previousByKey.find(key);
        if (previousEntry == previousByKey.end()) {
            plan.added.push_back(entry);
        } else if (previousEntry->second.source != entry.source ||
                   previousEntry->second.time != entry.time ||
                   previousEntry->second.target != entry.target) {
            plan.changed.push_back({previousEntry->second, entry});
        }
    }

    for (const auto& [key, entry] : previousByKey) {
        if (!desired.contains(key)) {
            plan.removed.push_back(entry);
        }
    }

    return plan;
}

DeploymentPlan planSingleMod(const fs::path& stagingPath, const fs::path& targetPath,
                             const ModSpec& mod, const Manifest& previous,
                             const PlanOptions& options, IndexStore& indexStore, bool enable) {
    const auto sourceName = mod.sourceName.empty() ? mod.installationPath : mod.sourceName;
    const auto single =
        planDeployment(stagingPath, targetPath, {mod}, previous, options, indexStore);

    std::map<std::string, DeployedFile> desired;
    for (const auto& file : previous.files) {
        if (file.source != sourceName) {
            desired[file_key(file, options.caseInsensitive)] = file;
        }
    }
    if (enable) {
        for (const auto& file : single.desired) {
            desired[file_key(file, options.caseInsensitive)] = file;
        }
    }

    std::map<std::string, DeployedFile> previousByKey;
    for (const auto& file : previous.files) {
        previousByKey[file_key(file, options.caseInsensitive)] = file;
    }

    DeploymentPlan plan;
    plan.desired.reserve(desired.size());
    for (const auto& [key, entry] : desired) {
        plan.desired.push_back(entry);
        const auto found = previousByKey.find(key);
        if (found == previousByKey.end()) {
            plan.added.push_back(entry);
        } else if (found->second.source != entry.source || found->second.time != entry.time ||
                   found->second.target != entry.target) {
            plan.changed.push_back({found->second, entry});
        }
    }
    for (const auto& [key, entry] : previousByKey) {
        if (!desired.contains(key)) {
            plan.removed.push_back(entry);
        }
    }
    return plan;
}

} // namespace vortex::deployment
