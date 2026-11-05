#include "vortex/deployment/executor.hpp"
#include "vortex/deployment/file_index.hpp"
#include "vortex/deployment/journal.hpp"
#include "vortex/deployment/manifest.hpp"
#include "vortex/deployment/platform.hpp"
#include "vortex/deployment/planner.hpp"
#include "vortex/core/install.hpp"
#include "vortex/core/status.hpp"

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

json dispatch(CoreState& state, const Request& request) {
    const auto& params = request.params;

    if (request.method == "ping") {
        return {
            {"version", CORE_VERSION},
            {"protocol", CORE_PROTOCOL},
            {"capabilities",
             {"ping", "scan", "staging.status", "install.apply", "plan", "deploy", "purge"}},
            {"pid", 0},
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
