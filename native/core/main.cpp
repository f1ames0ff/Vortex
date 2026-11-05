#include "vortex/deployment/executor.hpp"
#include "vortex/deployment/file_index.hpp"
#include "vortex/deployment/journal.hpp"
#include "vortex/deployment/manifest.hpp"
#include "vortex/deployment/platform.hpp"
#include "vortex/deployment/planner.hpp"

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

fs::path journalDirectory(const json& params, const fs::path& stagingPath) {
    const auto value = params.value("journalDir", std::string{});
    return value.empty() ? stagingPath / ".vortex-journal" : fs::path(value);
}

json dispatch(CoreState& state, const Request& request) {
    const auto& params = request.params;

    if (request.method == "ping") {
        return {{"version", CORE_VERSION}, {"pid", 0}};
    }

    if (request.method == "scan") {
        const auto stagingPath = fs::path(params.value("staging", std::string{}));
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

    if (request.method != "plan" && request.method != "deploy" &&
        request.method != "purge") {
        throw std::runtime_error("unknown method: " + request.method);
    }

    const auto stagingPath = fs::path(params.value("staging", std::string{}));
    const auto targetPath = fs::path(params.value("target", std::string{}));
    const auto manifestPath = fs::path(params.value("manifest", std::string{}));
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
    std::cout << response.dump() << '\n' << std::flush;
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
