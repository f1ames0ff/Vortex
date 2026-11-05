#include "vortex/deployment/executor.hpp"
#include "vortex/deployment/file_index.hpp"
#include "vortex/deployment/journal.hpp"
#include "vortex/deployment/manifest.hpp"
#include "vortex/deployment/platform.hpp"
#include "vortex/deployment/planner.hpp"
#include "vortex/core/archive.hpp"
#include "vortex/core/collection.hpp"
#include "vortex/core/deps.hpp"
#include "vortex/core/hash.hpp"
#include "vortex/core/install.hpp"
#include "vortex/core/plugins.hpp"
#include "vortex/core/status.hpp"
#include "vortex/game/descriptor.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using nlohmann::json;
using namespace vortex::deployment;

namespace {

constexpr const char* CORE_VERSION = "0.1.0";
constexpr int CORE_PROTOCOL = 2;

struct Request {
    json id;
    std::string method;
    json params;
};

struct CoreState {
    std::map<std::string, std::unique_ptr<IndexStore>> indexes;
    vortex::core::CollectionJobs collectionJobs;
};

IndexStore& indexFor(CoreState& state, const fs::path& stagingPath) {
    const auto key = stagingPath.lexically_normal().string();
    auto found = state.indexes.find(key);
    if (found == state.indexes.end()) {
        found = state.indexes.emplace(key, std::make_unique<IndexStore>(stagingPath)).first;
    }
    return *found->second;
}

std::vector<ModSpec> readMods(const json& params) {
    if (!params.contains("mods") || !params["mods"].is_array()) {
        throw std::runtime_error("params.mods must be an array");
    }
    std::vector<ModSpec> mods;
    for (const auto& item : params["mods"]) {
        if (!item.is_object()) {
            throw std::runtime_error("each mod must be an object");
        }
        ModSpec mod;
        mod.installationPath = item.value("installationPath", std::string{});
        mod.sourceName = item.value("sourceName", mod.installationPath);
        mod.target = item.value("target", std::string{});
        if (item.contains("fileOverrides") && item["fileOverrides"].is_array()) {
            mod.fileOverrides = item["fileOverrides"].get<std::vector<std::string>>();
        }
        if (mod.installationPath.empty()) {
            throw std::runtime_error("mod.installationPath must not be empty");
        }
        mods.push_back(std::move(mod));
    }
    return mods;
}

PlanOptions readOptions(const json& params) {
    PlanOptions options;
    if (params.contains("options") && params["options"].is_object()) {
        const auto& raw = params["options"];
        options.caseInsensitive = raw.value("caseInsensitive", false);
        if (raw.contains("blacklist") && raw["blacklist"].is_array()) {
            options.extraBlacklist = raw["blacklist"].get<std::vector<std::string>>();
        }
    }
    return options;
}

Manifest loadOrCreate(const fs::path& manifestPath, const json& params) {
    if (fs::exists(manifestPath)) {
        return loadManifest(manifestPath);
    }
    Manifest manifest;
    manifest.instance = params.value("instance", std::string{});
    manifest.stagingPath = params.value("staging", std::string{});
    manifest.targetPath = params.value("target", std::string{});
    return manifest;
}

json fileJson(const DeployedFile& file) {
    json entry = {
        {"relPath", file.relPath},
        {"source", file.source},
        {"time", file.time},
    };
    if (file.target.has_value()) {
        entry["target"] = *file.target;
    }
    if (!file.merged.empty()) {
        entry["merged"] = file.merged;
    }
    return entry;
}

json filesJson(const std::vector<DeployedFile>& files) {
    json result = json::array();
    for (const auto& file : files) {
        result.push_back(fileJson(file));
    }
    return result;
}

json failuresJson(const std::vector<OperationFailure>& failures) {
    json result = json::array();
    for (const auto& failure : failures) {
        result.push_back({
            {"source", failure.source},
            {"relPath", failure.relPath},
            {"outputPath", failure.outputPath},
            {"message", failure.message},
        });
    }
    return result;
}

json reportJson(const ApplyReport& report) {
    return {
        {"linked", report.linked},
        {"removed", report.removed},
        {"backedUp", report.backedUp},
        {"restored", report.restored},
        {"skipped", report.skipped},
        {"failures", failuresJson(report.failures)},
    };
}

json planJson(const DeploymentPlan& plan) {
    return {
        {"desired", plan.desired.size()},
        {"added", plan.added.size()},
        {"removed", plan.removed.size()},
        {"changed", plan.changed.size()},
    };
}

json installReportJson(const vortex::core::InstallReport& report) {
    json failures = json::array();
    for (const auto& failure : report.failures) {
        failures.push_back({
            {"source", failure.source},
            {"destination", failure.destination},
            {"message", failure.message},
        });
    }
    return {
        {"directories", report.directories},
        {"linked", report.linked},
        {"copied", report.copied},
        {"written", report.written},
        {"failures", std::move(failures)},
    };
}

json modStatusJson(const vortex::core::ModStatusResult& result) {
    json value = {
        {"id", result.id},
        {"installationPath", result.installationPath},
        {"status", vortex::core::statusName(result.status)},
        {"entries", result.entries},
    };
    if (!result.error.empty()) {
        value["error"] = result.error;
    }
    return value;
}

json gameJson(const vortex::game::GameDescriptor& game) {
    json value = {
        {"id", game.id},
        {"name", game.name},
        {"nexusDomain", game.nexusDomain},
        {"requiredFiles", game.requiredFiles},
        {"deploymentTargets", game.deploymentTargets},
        {"hooks", game.hooks},
        {"sourcePath", game.sourcePath.string()},
    };
    if (game.steamAppId.has_value()) {
        value["steamAppId"] = *game.steamAppId;
    }
    if (game.pluginPath.has_value()) {
        value["pluginPath"] = *game.pluginPath;
    }
    if (game.iniPath.has_value()) {
        value["iniPath"] = *game.iniPath;
    }
    return value;
}

json archiveEntryJson(const vortex::core::ArchiveEntry& entry) {
    return {
        {"path", entry.path},
        {"size", entry.size},
        {"directory", entry.directory},
        {"encrypted", entry.encrypted},
        {"symlink", entry.symlink},
    };
}

json archiveEntriesJson(const std::vector<vortex::core::ArchiveEntry>& entries) {
    json result = json::array();
    for (const auto& entry : entries) {
        result.push_back(archiveEntryJson(entry));
    }
    return result;
}

json stringsJson(const std::vector<std::string>& values) {
    json result = json::array();
    for (const auto& value : values) {
        result.push_back(value);
    }
    return result;
}

json pluginScanJson(const vortex::core::PluginScanResult& result) {
    json directories = json::array();
    for (const auto& directory : result.directories) {
        json files = json::array();
        for (const auto& file : directory.files) {
            files.push_back({{"name", file.name},
                             {"pluginId", file.pluginId},
                             {"ghost", file.ghost}});
        }
        json value = {{"path", directory.path}, {"files", std::move(files)}};
        if (!directory.error.empty()) {
            value["error"] = directory.error;
        }
        directories.push_back(std::move(value));
    }
    return {{"directories", std::move(directories)}};
}

json pluginOrderJson(const vortex::core::PluginOrderResult& result) {
    json plugins = json::array();
    for (const auto& row : result.plugins) {
        plugins.push_back(row);
    }
    json loadOrder = json::array();
    for (const auto& row : result.loadOrder) {
        loadOrder.push_back(row);
    }
    json value = {
        {"pluginsFile", result.pluginsFile},
        {"loadOrderFile", result.loadOrderFile},
        {"pluginsHeader", result.pluginsHeader},
        {"loadOrderHeader", result.loadOrderHeader},
        {"plugins", std::move(plugins)},
        {"loadOrder", std::move(loadOrder)},
    };
    if (!result.error.empty()) {
        value["error"] = result.error;
    }
    return value;
}

vortex::core::InstallInstructionType parseInstructionType(const std::string& value) {
    if (value == "mkdir") {
        return vortex::core::InstallInstructionType::Mkdir;
    }
    if (value == "copy") {
        return vortex::core::InstallInstructionType::Copy;
    }
    if (value == "hardlink") {
        return vortex::core::InstallInstructionType::Hardlink;
    }
    if (value == "write") {
        return vortex::core::InstallInstructionType::Write;
    }
    throw std::runtime_error("unknown install instruction type: " + value);
}

std::vector<vortex::core::InstallInstruction> readInstructions(const json& params) {
    if (!params.contains("instructions") || !params["instructions"].is_array()) {
        throw std::runtime_error("params.instructions must be an array");
    }
    std::vector<vortex::core::InstallInstruction> instructions;
    instructions.reserve(params["instructions"].size());
    for (const auto& item : params["instructions"]) {
        if (!item.is_object()) {
            throw std::runtime_error("each install instruction must be an object");
        }
        vortex::core::InstallInstruction instruction;
        instruction.type = parseInstructionType(item.value("type", std::string{}));
        instruction.source = item.value("source", std::string{});
        instruction.destination = item.value("destination", std::string{});
        instruction.data = item.value("data", std::string{});
        instruction.optional = item.value("optional", false);
        if (instruction.destination.empty()) {
            throw std::runtime_error("install instruction.destination must not be empty");
        }
        instructions.push_back(std::move(instruction));
    }
    return instructions;
}

fs::path pathFromUtf8(const std::string& value) {
    const auto* begin = reinterpret_cast<const char8_t*>(value.data());
    return fs::path(std::u8string(begin, begin + value.size()));
}

fs::path journalDirectory(const json& params, const fs::path& stagingPath) {
    const auto value = params.value("journalDir", std::string{});
    return value.empty() ? stagingPath / ".vortex-journal" : pathFromUtf8(value);
}

std::optional<std::string> optString(const json& object, const char* key) {
    if (!object.is_object() || !object.contains(key) || object[key].is_null()) {
        return std::nullopt;
    }
    if (object[key].is_string()) {
        return object[key].get<std::string>();
    }
    if (object[key].is_number_integer()) {
        return std::to_string(object[key].get<std::int64_t>());
    }
    return std::nullopt;
}

std::vector<std::string> stringArray(const json& object, const char* key) {
    std::vector<std::string> result;
    if (!object.is_object() || !object.contains(key) || !object[key].is_array()) {
        return result;
    }
    for (const auto& item : object[key]) {
        if (item.is_string()) {
            result.push_back(item.get<std::string>());
        }
    }
    return result;
}

vortex::core::ModReference parseReference(const json& value) {
    vortex::core::ModReference reference;
    if (!value.is_object()) {
        return reference;
    }
    reference.id = optString(value, "id");
    reference.idHint = optString(value, "idHint");
    reference.md5Hint = optString(value, "md5Hint");
    reference.archiveId = optString(value, "archiveId");
    reference.fileMD5 = optString(value, "fileMD5");
    reference.fileExpression = optString(value, "fileExpression");
    reference.logicalFileName = optString(value, "logicalFileName");
    reference.tag = optString(value, "tag");
    reference.gameId = optString(value, "gameId");
    reference.versionMatch = optString(value, "versionMatch");
    if (value.contains("repo") && value["repo"].is_object()) {
        vortex::core::RepoReference repo;
        repo.repository = value["repo"].value("repository", std::string{});
        repo.modId = optString(value["repo"], "modId");
        repo.fileId = optString(value["repo"], "fileId");
        reference.repo = std::move(repo);
    }
    return reference;
}

vortex::core::LookupMod parseLookupMod(const json& value, std::size_t fallbackIndex) {
    vortex::core::LookupMod mod;
    if (!value.is_object()) {
        return mod;
    }
    const auto& attrs =
        value.contains("attributes") && value["attributes"].is_object() ? value["attributes"]
                                                                       : value;
    mod.id = value.value("id", "mod-" + std::to_string(fallbackIndex));
    mod.fileMD5 = optString(attrs, "fileMD5");
    mod.fileName = optString(attrs, "fileName");
    mod.name = optString(attrs, "name");
    mod.logicalFileName = optString(attrs, "logicalFileName");
    mod.customFileName = optString(attrs, "customFileName");
    mod.version = optString(attrs, "version");
    mod.fileId = optString(attrs, "fileId");
    mod.modId = optString(attrs, "modId");
    mod.source = optString(attrs, "source");
    mod.referenceTag = optString(attrs, "referenceTag");
    mod.state = optString(value, "state");
    mod.additionalLogicalFileNames = stringArray(attrs, "additionalLogicalFileNames");
    mod.referenceTags = stringArray(attrs, "referenceTags");
    mod.game = stringArray(attrs, "game");
    if (attrs.contains("installerChoices")) {
        mod.installerChoices = attrs["installerChoices"];
    }
    if (attrs.contains("fileList")) {
        mod.fileList = attrs["fileList"];
    }
    if (attrs.contains("patches")) {
        mod.patches = attrs["patches"];
    }
    return mod;
}

vortex::core::DownloadInfo parseDownload(const json& value, std::size_t fallbackIndex) {
    vortex::core::DownloadInfo download;
    if (!value.is_object()) {
        return download;
    }
    download.id = value.value("id", "download-" + std::to_string(fallbackIndex));
    download.fileMD5 = optString(value, "fileMD5");
    download.localPath = optString(value, "localPath");
    download.logicalFileName = optString(value, "logicalFileName");
    download.version = optString(value, "version");
    download.fileId = optString(value, "fileId");
    download.modId = optString(value, "modId");
    download.source = optString(value, "source");
    download.referenceTag = optString(value, "referenceTag");
    download.referenceTags = stringArray(value, "referenceTags");
    download.game = stringArray(value, "game");
    download.state = value.value("state", std::string("finished"));
    download.fileTime = value.value("fileTime", std::int64_t{0});

    if (value.contains("modInfo") && value["modInfo"].is_object()) {
        const auto& modInfo = value["modInfo"];
        if (!download.localPath.has_value()) {
            download.localPath = optString(modInfo, "name");
        }
        if (!download.logicalFileName.has_value()) {
            download.logicalFileName = optString(modInfo, "name");
        }
        if (!download.version.has_value()) {
            download.version = optString(modInfo, "version");
            if (!download.version.has_value() && modInfo.contains("meta") &&
                modInfo["meta"].is_object()) {
                download.version = optString(modInfo["meta"], "fileVersion");
            }
        }
        if (!download.source.has_value()) {
            download.source = optString(modInfo, "source");
        }
        download.referenceTag = optString(modInfo, "referenceTag");
        download.referenceTags = stringArray(modInfo, "referenceTags");
        if (modInfo.contains("nexus") && modInfo["nexus"].is_object() &&
            modInfo["nexus"].contains("ids") && modInfo["nexus"]["ids"].is_object()) {
            download.modId = optString(modInfo["nexus"]["ids"], "modId");
            download.fileId = optString(modInfo["nexus"]["ids"], "fileId");
        }
        if (modInfo.contains("meta") && modInfo["meta"].is_object() &&
            modInfo["meta"].contains("details") && modInfo["meta"]["details"].is_object()) {
            if (!download.modId.has_value()) {
                download.modId = optString(modInfo["meta"]["details"], "modId");
            }
            if (!download.fileId.has_value()) {
                download.fileId = optString(modInfo["meta"]["details"], "fileId");
            }
        }
    }
    return download;
}

vortex::core::RuleInfo parseRule(const json& value, std::size_t fallbackIndex) {
    vortex::core::RuleInfo rule;
    if (!value.is_object()) {
        return rule;
    }
    rule.index = value.value("index", static_cast<int>(fallbackIndex));
    rule.reference = parseReference(value.contains("reference") ? value["reference"] : json{});
    rule.type = value.value("type", std::string{});
    rule.ignored = value.value("ignored", false);
    rule.phase = value.value("phase", 0);
    if (value.contains("extra") && value["extra"].is_object()) {
        if (!value.contains("phase")) {
            rule.phase = value["extra"].value("phase", 0);
        }
        rule.localPath = optString(value["extra"], "localPath");
        if (!value.contains("patches") && value["extra"].contains("patches")) {
            rule.patches = value["extra"]["patches"];
        }
    }
    if (rule.type == "recommends") {
        rule.phase = 666;
    }
    if (value.contains("installerChoices")) {
        rule.installerChoices = value["installerChoices"];
    }
    if (value.contains("fileList")) {
        rule.fileList = value["fileList"];
    }
    if (value.contains("patches")) {
        rule.patches = value["patches"];
    }
    return rule;
}

std::vector<vortex::core::RuleInfo> readRules(const json& params) {
    if (!params.contains("rules") || !params["rules"].is_array()) {
        throw std::runtime_error("params.rules must be an array");
    }
    std::vector<vortex::core::RuleInfo> rules;
    rules.reserve(params["rules"].size());
    for (std::size_t index = 0; index < params["rules"].size(); ++index) {
        rules.push_back(parseRule(params["rules"][index], index));
    }
    return rules;
}

std::vector<vortex::core::LookupMod> readLookupMods(const json& params) {
    std::vector<vortex::core::LookupMod> mods;
    if (!params.contains("mods") || !params["mods"].is_array()) {
        return mods;
    }
    mods.reserve(params["mods"].size());
    for (std::size_t index = 0; index < params["mods"].size(); ++index) {
        mods.push_back(parseLookupMod(params["mods"][index], index));
    }
    return mods;
}

std::vector<vortex::core::DownloadInfo> readDownloads(const json& params) {
    std::vector<vortex::core::DownloadInfo> downloads;
    if (!params.contains("downloads") || !params["downloads"].is_array()) {
        return downloads;
    }
    downloads.reserve(params["downloads"].size());
    for (std::size_t index = 0; index < params["downloads"].size(); ++index) {
        downloads.push_back(parseDownload(params["downloads"][index], index));
    }
    return downloads;
}

json dependencyJson(const vortex::core::DependencyEntry& entry) {
    json value = {
        {"ruleIndex", entry.ruleIndex},
        {"ruleId", entry.ruleId},
        {"type", entry.type},
        {"phase", entry.phase},
        {"ignored", entry.ignored},
        {"status", entry.status},
    };
    if (entry.modId.has_value()) {
        value["modId"] = *entry.modId;
    }
    if (entry.downloadId.has_value()) {
        value["downloadId"] = *entry.downloadId;
    }
    return value;
}

json dependenciesJson(const std::vector<vortex::core::DependencyEntry>& entries) {
    json result = json::array();
    for (const auto& entry : entries) {
        result.push_back(dependencyJson(entry));
    }
    return result;
}

json collectionMemberJson(const vortex::core::CollectionMemberState& member) {
    json value = {
        {"ruleIndex", member.ruleIndex},
        {"ruleId", member.ruleId},
        {"type", member.type},
        {"phase", member.phase},
        {"ignored", member.ignored},
        {"status", member.status},
        {"attempts", member.attempts},
    };
    if (member.modId.has_value()) {
        value["modId"] = *member.modId;
    }
    if (member.downloadId.has_value()) {
        value["downloadId"] = *member.downloadId;
    }
    if (!member.error.empty()) {
        value["error"] = member.error;
    }
    return value;
}

std::vector<vortex::core::CollectionMemberState> readCollectionMembers(const json& params) {
    if (!params.contains("members") || !params["members"].is_array()) {
        throw std::runtime_error("params.members must be an array");
    }
    std::vector<vortex::core::CollectionMemberState> members;
    members.reserve(params["members"].size());
    for (const auto& item : params["members"]) {
        if (!item.is_object()) {
            throw std::runtime_error("each collection member must be an object");
        }
        vortex::core::CollectionMemberState member;
        member.ruleIndex = item.value("ruleIndex", 0);
        member.ruleId = item.value("ruleId", std::string{});
        member.type = item.value("type", std::string{});
        member.phase = item.value("phase", 0);
        member.ignored = item.value("ignored", false);
        member.status = item.value("status", std::string("pending"));
        member.modId = optString(item, "modId");
        member.downloadId = optString(item, "downloadId");
        if (member.ruleId.empty()) {
            throw std::runtime_error("collection member.ruleId is required");
        }
        members.push_back(std::move(member));
    }
    return members;
}

json collectionPlanJson(const vortex::core::CollectionPlan& plan) {
    json phases = json::array();
    for (const auto phase : plan.phases) {
        phases.push_back(phase);
    }
    return {
        {"members", dependenciesJson(plan.members)},
        {"phases", std::move(phases)},
        {"totalRequired", plan.totalRequired},
        {"totalOptional", plan.totalOptional},
    };
}

json dispatch(CoreState& state, const Request& request) {
    const auto& params = request.params;

    if (request.method == "ping") {
        return {
            {"version", CORE_VERSION},
            {"protocol", CORE_PROTOCOL},
            {"capabilities",
             {"ping", "scan", "staging.status", "install.apply", "game.registry",
              "archive.list", "archive.extract", "hash.files", "deps.matchMod",
              "deps.matchDownload", "deps.resolve", "collection.plan", "collection.start",
              "collection.status", "collection.next", "collection.member", "collection.pause",
              "collection.cancel", "plugins.scan", "plugins.loadOrder.read",
              "plugins.loadOrder.write", "plan", "deploy", "deploy.symlink", "purge"}},
            {"pid", 0},
        };
    }

    if (request.method == "game.registry") {
        const auto directory = params.value("directory", std::string{});
        if (directory.empty()) {
            throw std::runtime_error("params.directory is required");
        }
        const auto games = vortex::game::loadGameDescriptors(pathFromUtf8(directory));
        json result = json::array();
        for (const auto& game : games) {
            result.push_back(gameJson(game));
        }
        return {{"games", std::move(result)}};
    }

    if (request.method == "plugins.scan") {
        if (!params.contains("directories") || !params["directories"].is_array()) {
            throw std::runtime_error("params.directories must be an array");
        }
        std::vector<vortex::core::PluginScanRequest> requests;
        requests.reserve(params["directories"].size());
        for (const auto& item : params["directories"]) {
            if (!item.is_object()) {
                throw std::runtime_error("each plugin scan directory must be an object");
            }
            vortex::core::PluginScanRequest scan;
            scan.path = pathFromUtf8(item.value("path", std::string{}));
            if (scan.path.empty()) {
                throw std::runtime_error("plugin scan directory.path is required");
            }
            scan.extensions = stringArray(item, "extensions");
            if (scan.extensions.empty()) {
                scan.extensions = {".esm", ".esp"};
            }
            requests.push_back(std::move(scan));
        }
        return pluginScanJson(vortex::core::scanPlugins(requests));
    }

    if (request.method == "plugins.loadOrder.read" || request.method == "plugins.loadOrder.write") {
        const auto directory = pathFromUtf8(params.value("directory", std::string{}));
        if (directory.empty()) {
            throw std::runtime_error("params.directory is required");
        }
        if (request.method == "plugins.loadOrder.read") {
            return pluginOrderJson(vortex::core::readPluginOrder(directory));
        }

        if (!params.contains("plugins") || !params["plugins"].is_array() ||
            !params.contains("loadOrder") || !params["loadOrder"].is_array()) {
            throw std::runtime_error("params.plugins and params.loadOrder must be arrays");
        }
        std::vector<std::string> plugins;
        for (const auto& row : params["plugins"]) {
            if (!row.is_string()) {
                throw std::runtime_error("plugin rows must be strings");
            }
            plugins.push_back(row.get<std::string>());
        }
        std::vector<std::string> loadOrder;
        for (const auto& row : params["loadOrder"]) {
            if (!row.is_string()) {
                throw std::runtime_error("load order rows must be strings");
            }
            loadOrder.push_back(row.get<std::string>());
        }
        return pluginOrderJson(vortex::core::writePluginOrder(directory, plugins, loadOrder));
    }

    if (request.method == "deps.matchMod") {
        const auto reference = parseReference(params.contains("reference") ? params["reference"]
                                                                           : json{});
        const auto mods = readLookupMods(params);
        vortex::core::RuleInfo spec;
        bool hasSpec = false;
        if (params.contains("installSpec") && params["installSpec"].is_object()) {
            const auto& raw = params["installSpec"];
            hasSpec = true;
            if (raw.contains("installerChoices")) {
                spec.installerChoices = raw["installerChoices"];
            }
            if (raw.contains("fileList")) {
                spec.fileList = raw["fileList"];
            }
            if (raw.contains("patches")) {
                spec.patches = raw["patches"];
            }
        }
        const auto modId = vortex::core::matchModId(mods, reference, hasSpec ? &spec : nullptr);
        if (modId.has_value()) {
            return {{"modId", *modId}};
        }
        return json::object();
    }

    if (request.method == "deps.matchDownload") {
        const auto reference = parseReference(params.contains("reference") ? params["reference"]
                                                                           : json{});
        const auto downloads = readDownloads(params);
        const auto downloadId = vortex::core::matchDownloadId(downloads, reference);
        if (downloadId.has_value()) {
            return {{"downloadId", *downloadId}};
        }
        return json::object();
    }

    if (request.method == "deps.resolve" || request.method == "collection.plan") {
        const auto rules = readRules(params);
        const auto mods = readLookupMods(params);
        const auto downloads = readDownloads(params);
        if (request.method == "deps.resolve") {
            const auto members = vortex::core::resolveDependencies(rules, mods, downloads);
            return {{"dependencies", dependenciesJson(members)}};
        }
        const auto plan = vortex::core::planCollection(rules, mods, downloads);
        return collectionPlanJson(plan);
    }

    if (request.method == "collection.start") {
        vortex::core::CollectionJob job;
        job.collectionId = params.value("collectionId", std::string{});
        job.gameId = params.value("gameId", std::string{});
        job.concurrency = params.value("concurrency", 5);
        job.maxRetries = params.value("maxRetries", 3);
        job.stallMs = params.value("stallMs", 60000);
        job.members = readCollectionMembers(params);
        const auto jobId = state.collectionJobs.start(std::move(job));
        const auto* started = state.collectionJobs.find(jobId);
        return {
            {"jobId", jobId},
            {"status", vortex::core::CollectionJobs::status(*started)},
        };
    }

    if (request.method == "collection.status" || request.method == "collection.next" ||
        request.method == "collection.pause" || request.method == "collection.cancel" ||
        request.method == "collection.member") {
        const auto jobId = params.value("jobId", std::string{});
        if (jobId.empty()) {
            throw std::runtime_error("params.jobId is required");
        }
        auto* job = state.collectionJobs.find(jobId);
        if (job == nullptr) {
            throw std::runtime_error("unknown collection job: " + jobId);
        }

        if (request.method == "collection.status") {
            return vortex::core::CollectionJobs::status(*job);
        }
        if (request.method == "collection.pause") {
            state.collectionJobs.pause(jobId);
            return vortex::core::CollectionJobs::status(*job);
        }
        if (request.method == "collection.cancel") {
            state.collectionJobs.cancel(jobId);
            return vortex::core::CollectionJobs::status(*job);
        }
        if (request.method == "collection.member") {
            vortex::core::CollectionMemberUpdate update;
            update.ruleId = params.value("ruleId", std::string{});
            update.status = params.value("status", std::string{});
            update.modId = optString(params, "modId");
            update.downloadId = optString(params, "downloadId");
            update.error = params.value("error", std::string{});
            update.retry = params.value("retry", false);
            state.collectionJobs.update(jobId, update);
            return vortex::core::CollectionJobs::status(*job);
        }

        const auto selected = state.collectionJobs.next(jobId);
        json members = json::array();
        for (const auto& member : selected) {
            members.push_back(collectionMemberJson(member));
        }
        return {
            {"members", std::move(members)},
            {"status", vortex::core::CollectionJobs::status(*job)},
        };
    }

    if (request.method == "scan") {
        const auto stagingPath = pathFromUtf8(params.value("staging", std::string{}));
        if (stagingPath.empty()) {
            throw std::runtime_error("params.staging is required");
        }
        auto& index = indexFor(state, stagingPath);
        std::size_t folders = 0;
        std::size_t files = 0;
        for (const auto& entry : fs::directory_iterator(stagingPath)) {
            if (!entry.is_directory()) {
                continue;
            }
            const auto& mod = index.get(entry.path());
            ++folders;
            files += mod.files.size();
        }
        index.flush();
        return {{"mods", folders}, {"files", files}};
    }

    if (request.method == "archive.list" || request.method == "archive.extract") {
        const auto archive = pathFromUtf8(params.value("archive", std::string{}));
        const auto sevenZip = pathFromUtf8(params.value("sevenZip", std::string{}));
        if (archive.empty() || sevenZip.empty()) {
            throw std::runtime_error("params.archive and params.sevenZip are required");
        }
        const auto password = params.value("password", std::string{});

        if (request.method == "archive.list") {
            const auto listing = vortex::core::listArchive(archive, sevenZip, password);
            return {
                {"code", listing.code},
                {"entries", archiveEntriesJson(listing.entries)},
                {"errors", stringsJson(listing.errors)},
                {"encrypted", listing.encrypted},
            };
        }

        const auto destination = pathFromUtf8(params.value("destination", std::string{}));
        if (destination.empty()) {
            throw std::runtime_error("params.destination is required");
        }
        const auto extraction =
            vortex::core::extractArchive(archive, destination, sevenZip, password);
        return {
            {"code", extraction.code},
            {"entries", archiveEntriesJson(extraction.entries)},
            {"errors", stringsJson(extraction.errors)},
        };
    }

    if (request.method == "hash.files") {
        if (!params.contains("files") || !params["files"].is_array()) {
            throw std::runtime_error("params.files must be an array");
        }
        std::vector<std::string> files;
        files.reserve(params["files"].size());
        for (const auto& item : params["files"]) {
            if (!item.is_string()) {
                throw std::runtime_error("each file must be a string");
            }
            files.push_back(item.get<std::string>());
        }

        const auto algorithm =
            vortex::core::parseHashAlgorithm(params.value("algorithm", std::string("md5")));
        const auto concurrency = params.value("concurrency", std::size_t{8});
        const auto hashes = vortex::core::hashFiles(files, algorithm, concurrency);

        json results = json::array();
        for (const auto& result : hashes.results) {
            if (result.path.empty()) {
                continue;
            }
            results.push_back({
                {"path", result.path},
                {"hash", result.hash},
                {"bytes", result.bytes},
            });
        }
        json failures = json::array();
        for (const auto& failure : hashes.failures) {
            failures.push_back({{"path", failure.path}, {"message", failure.message}});
        }
        return {
            {"algorithm", vortex::core::hashAlgorithmName(algorithm)},
            {"results", std::move(results)},
            {"failures", std::move(failures)},
        };
    }

    if (request.method == "staging.status") {
        const auto stagingPath = pathFromUtf8(params.value("staging", std::string{}));
        if (stagingPath.empty()) {
            throw std::runtime_error("params.staging is required");
        }
        if (!params.contains("mods") || !params["mods"].is_array()) {
            throw std::runtime_error("params.mods must be an array");
        }

        std::vector<vortex::core::ModStatusQuery> queries;
        queries.reserve(params["mods"].size());
        for (const auto& item : params["mods"]) {
            if (!item.is_object()) {
                throw std::runtime_error("each mod must be an object");
            }
            vortex::core::ModStatusQuery query;
            query.installationPath = item.value("installationPath", std::string{});
            query.id = item.value("id", query.installationPath);
            if (query.installationPath.empty()) {
                throw std::runtime_error("mod.installationPath must not be empty");
            }
            queries.push_back(std::move(query));
        }

        const auto results = vortex::core::inspectMods(stagingPath, queries);
        json mods = json::array();
        for (const auto& result : results) {
            mods.push_back(modStatusJson(result));
        }
        return {{"mods", std::move(mods)}};
    }

    if (request.method == "install.apply") {
        vortex::core::InstallRequest install;
        install.tempPath = pathFromUtf8(params.value("temp", std::string{}));
        install.destinationPath = pathFromUtf8(params.value("destination", std::string{}));
        if (install.tempPath.empty() || install.destinationPath.empty()) {
            throw std::runtime_error("params.temp and params.destination are required");
        }
        install.instructions = readInstructions(params);
        install.operationId = platform::operation_id();
        install.concurrency = params.value("concurrency", std::size_t{8});

        const auto journalDirectory = params.value("journalDir", std::string{});
        const auto journalRoot = journalDirectory.empty()
                                     ? install.destinationPath.parent_path() /
                                           ".vortex-install-journal"
                                     : pathFromUtf8(journalDirectory);
        install.journalPath = journalRoot / (install.operationId + ".jsonl");

        const auto report = vortex::core::applyInstall(install);
        return {
            {"report", installReportJson(report)},
            {"operation", install.operationId},
        };
    }

    if (request.method != "plan" && request.method != "deploy" &&
        request.method != "purge") {
        throw std::runtime_error("unknown method: " + request.method);
    }

    const auto stagingPath = pathFromUtf8(params.value("staging", std::string{}));
    const auto targetPath = pathFromUtf8(params.value("target", std::string{}));
    const auto manifestPath = pathFromUtf8(params.value("manifest", std::string{}));
    if (stagingPath.empty() || targetPath.empty() || manifestPath.empty()) {
        throw std::runtime_error("params.staging, params.target and params.manifest are required");
    }

    if (request.method == "purge") {
        const auto manifest = loadManifest(manifestPath);
        const auto operation = platform::operation_id();
        Journal journal(journalDirectory(params, stagingPath) / (operation + ".jsonl"), operation);
        ExecutorContext context;
        context.stagingPath = stagingPath;
        context.targetPath = targetPath;
        context.manifestPath = manifestPath;
        context.previous = manifest;
        context.instance = params.value("instance", manifest.instance);
        context.deploymentMethod = params.value("method", std::string("hardlink"));
        context.gameId = params.value("game", manifest.gameId.value_or(""));
        context.caseInsensitive =
            params.value("caseInsensitive", platform::is_case_insensitive(targetPath));
        const auto report = purgeDeployment(manifest, context, journal);
        return {
            {"report", reportJson(report)},
            {"operation", operation},
            {"files", filesJson(loadManifest(manifestPath).files)},
        };
    }

    auto previous = loadOrCreate(manifestPath, params);
    const auto mods = readMods(params);
    const auto options = readOptions(params);
    auto& index = indexFor(state, stagingPath);
    const auto plan = planDeployment(stagingPath, targetPath, mods, previous, options, index);
    index.flush();

    if (request.method == "plan") {
        json result = planJson(plan);
        result["operation"] = platform::operation_id();
        if (params.value("includeFiles", false)) {
            result["files"] = filesJson(plan.desired);
        }
        return result;
    }

    const auto operation = platform::operation_id();
    Journal journal(journalDirectory(params, stagingPath) / (operation + ".jsonl"), operation);
    ExecutorContext context;
    context.stagingPath = stagingPath;
    context.targetPath = targetPath;
    context.manifestPath = manifestPath;
    context.previous = std::move(previous);
    context.instance = params.value("instance", context.previous.instance);
    context.deploymentMethod = params.value("method", std::string("hardlink"));
    context.gameId = params.value("game", context.previous.gameId.value_or(""));
    context.caseInsensitive =
        options.caseInsensitive || platform::is_case_insensitive(targetPath);

    const auto report = applyDeployment(plan, context, journal);

    json result = {
        {"report", reportJson(report)},
        {"operation", operation},
        {"plan", planJson(plan)},
    };
    if (params.value("includeFiles", false)) {
        // The manifest holds the committed state after the partial-failure
        // handling, so it is the accurate list to hand back to the renderer.
        result["files"] = filesJson(loadManifest(manifestPath).files);
    }
    return result;
}

void writeResponse(const json& response) {
    std::cout << response.dump(-1, ' ', false, json::error_handler_t::replace) << '\n'
              << std::flush;
}

} // namespace

int main() {
    std::ios::sync_with_stdio(false);
    CoreState state;

    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.empty()) {
            continue;
        }

        Request request;
        try {
            const auto value = json::parse(line);
            request.id = value.value("id", json{});
            request.method = value.value("method", std::string{});
            request.params = value.value("params", json::object());
            if (request.method.empty()) {
                throw std::runtime_error("method is required");
            }
            if (request.method == "shutdown") {
                writeResponse({{"id", request.id}, {"result", {{"ok", true}}}});
                return 0;
            }
            writeResponse({{"id", request.id}, {"result", dispatch(state, request)}});
        } catch (const std::exception& err) {
            writeResponse({
                {"id", request.id},
                {"error", {{"code", "internal"}, {"message", err.what()}}},
            });
        }
    }
    return 0;
}
