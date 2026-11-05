#include "vortex/deployment/executor.hpp"
#include "vortex/deployment/file_index.hpp"
#include "vortex/deployment/journal.hpp"
#include "vortex/deployment/manifest.hpp"
#include "vortex/deployment/platform.hpp"
#include "vortex/deployment/planner.hpp"
#include "vortex/core/install.hpp"
#include "vortex/core/status.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using namespace vortex::deployment;

namespace {

void check(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void writeFile(const fs::path& path, const std::string& content) {
    fs::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw std::runtime_error("failed to write " + path.string());
    }
    output << content;
}

std::string readFile(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("failed to read " + path.string());
    }
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

fs::path makeRoot(const std::string& name) {
    const auto root = fs::temp_directory_path() / ("vortex-native-" + name + "-" +
                                                   platform::operation_id());
    fs::create_directories(root);
    return root;
}

void testManifestRoundtrip() {
    const auto root = makeRoot("manifest");
    const auto path = root / "vortex.deployment.json";

    Manifest manifest;
    manifest.instance = "instance";
    manifest.deploymentMethod = "hardlink";
    manifest.gameId = "skyrimse";
    manifest.stagingPath = "/staging";
    manifest.targetPath = "/target";
    manifest.deploymentTime = 42;
    manifest.files.push_back({"a.txt", "mod-a", std::nullopt, 123, {"mod-a"}});
    manifest.files.push_back({"b.txt", "mod-b", "Data", 456, {}});

    saveManifestAtomic(path, manifest);
    const auto loaded = loadManifest(path);

    check(loaded.instance == manifest.instance, "manifest instance");
    check(loaded.deploymentMethod == manifest.deploymentMethod, "manifest method");
    check(loaded.files.size() == 2, "manifest file count");
    check(loaded.files[0].relPath == "a.txt", "manifest relPath");
    check(loaded.files[0].source == "mod-a", "manifest source");
    check(loaded.files[0].time == 123, "manifest time");
    check(loaded.files[0].merged.size() == 1, "manifest merged");
    check(loaded.files[1].target == "Data", "manifest target");

    fs::remove_all(root);
}

void testIndexCachesAndInvalidates() {
    const auto root = makeRoot("index");
    const auto mod = root / "ModA";
    writeFile(mod / "one.txt", "one");

    IndexStore store(root);
    const auto& first = store.get(mod);
    check(first.files.size() == 1, "first index has one file");

    const auto& cached = store.get(mod);
    check(&cached == &first, "index cache should reuse the entry");

    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    writeFile(mod / "two.txt", "two");

    const auto& second = store.get(mod);
    check(second.files.size() == 2, "index should rescan after directory change");
    store.flush();

    IndexStore reloaded(root);
    const auto& persisted = reloaded.get(mod);
    check(persisted.files.size() == 2, "persisted index should load");

    fs::remove_all(root);
}

void testPlannerDiff() {
    const auto root = makeRoot("planner");
    const auto staging = root / "staging";
    const auto target = root / "target";
    const auto mod = staging / "ModA";
    writeFile(mod / "a.txt", "a");
    writeFile(mod / "nested" / "b.txt", "b");
    writeFile(mod / ".git" / "config", "ignored");
    writeFile(mod / "meta.ini", "ignored");

    const std::vector<ModSpec> mods = {{"ModA", "ModA", "", {}}};
    const PlanOptions options;
    IndexStore index(staging);

    Manifest previous;
    const auto first = planDeployment(staging, target, mods, previous, options, index);
    check(first.desired.size() == 2, "planner should keep two files");
    check(first.added.size() == 2, "planner should see two additions");

    Manifest manifest;
    manifest.files = first.desired;
    const auto unchanged = planDeployment(staging, target, mods, manifest, options, index);
    check(unchanged.added.empty() && unchanged.changed.empty() && unchanged.removed.empty(),
          "planner should detect an unchanged deployment");

    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    writeFile(mod / "a.txt", "changed");
    const auto changed = planDeployment(staging, target, mods, manifest, options, index);
    check(changed.changed.size() == 1, "planner should detect a changed file");

    fs::remove(mod / "nested" / "b.txt");
    const auto removed = planDeployment(staging, target, mods, manifest, options, index);
    check(removed.removed.size() == 1, "planner should detect a removed file");

    fs::remove_all(root);
}

void testDeployAndPurge() {
    const auto root = makeRoot("deploy");
    const auto staging = root / "staging";
    const auto target = root / "target";
    const auto manifestPath = root / "manifest.json";
    const auto mod = staging / "ModA";
    writeFile(mod / "a.txt", "mod-a");
    writeFile(target / "a.txt", "game-a");

    const std::vector<ModSpec> mods = {{"ModA", "ModA", "", {}}};
    const PlanOptions planOptions;
    IndexStore index(staging);
    auto plan = planDeployment(staging, target, mods, Manifest{}, planOptions, index);
    check(plan.desired.size() == 1, "deploy plan size");

    const auto operation = platform::operation_id();
    Journal journal(root / "journal.jsonl", operation);
    ExecutorContext context;
    context.stagingPath = staging;
    context.targetPath = target;
    context.manifestPath = manifestPath;
    context.instance = "test";
    context.deploymentMethod = "hardlink";
    context.gameId = "skyrimse";

    const auto report = applyDeployment(plan, context, journal);
    check(report.failures.empty(), "deploy should not fail");
    check(readFile(target / "a.txt") == "mod-a", "deployed content");
    check(readFile(target / "a.txt.vortex_backup") == "game-a", "game backup");

    const auto manifest = loadManifest(manifestPath);
    check(manifest.files.size() == 1, "manifest after deploy");

    IndexStore secondIndex(staging);
    const auto noop = planDeployment(staging, target, mods, manifest, planOptions, secondIndex);
    check(noop.added.empty() && noop.changed.empty() && noop.removed.empty(),
          "no-op deployment plan");

    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    writeFile(mod / "a.txt", "mod-a-v2");
    IndexStore changedIndex(staging);
    const auto changed = planDeployment(staging, target, mods, manifest, planOptions, changedIndex);
    check(changed.changed.size() == 1, "changed deployment plan");

    context.previous = manifest;
    applyDeployment(changed, context, journal);
    check(readFile(target / "a.txt") == "mod-a-v2", "changed deployed content");
    check(readFile(target / "a.txt.vortex_backup") == "game-a", "backup survives replacement");

    const auto changedManifest = loadManifest(manifestPath);
    Journal purgeJournal(root / "purge.jsonl", platform::operation_id());
    const auto purgeReport = purgeDeployment(changedManifest, context, purgeJournal);
    check(purgeReport.failures.empty(), "purge should not fail");
    check(readFile(target / "a.txt") == "game-a", "purge restores game file");
    check(!fs::exists(target / "a.txt.vortex_backup"), "purge removes backup");
    check(loadManifest(manifestPath).files.empty(), "purge clears manifest");

    fs::remove_all(root);
}

void testPurgeKeepsFailedEntries() {
    const auto root = makeRoot("purge-failure");
    const auto staging = root / "staging";
    const auto target = root / "target";
    const auto manifestPath = root / "manifest.json";
    const auto mod = staging / "ModA";
    writeFile(mod / "a.txt", "mod-a");
    writeFile(target / "a.txt", "game-a");

    const std::vector<ModSpec> mods = {{"ModA", "ModA", "", {}}};
    const PlanOptions planOptions;
    IndexStore index(staging);
    auto plan = planDeployment(staging, target, mods, Manifest{}, planOptions, index);

    Journal journal(root / "journal.jsonl", platform::operation_id());
    ExecutorContext context;
    context.stagingPath = staging;
    context.targetPath = target;
    context.manifestPath = manifestPath;
    context.instance = "test";
    applyDeployment(plan, context, journal);
    check(loadManifest(manifestPath).files.size() == 1, "manifest before failed purge");

    // Simulate the user replacing the deployed hardlink with their own file.
    fs::remove(target / "a.txt");
    writeFile(target / "a.txt", "user-change");

    Journal purgeJournal(root / "purge.jsonl", platform::operation_id());
    const auto report = purgeDeployment(loadManifest(manifestPath), context, purgeJournal);
    check(report.failures.size() == 1, "modified output must fail the purge");
    check(readFile(target / "a.txt") == "user-change", "modified output is left alone");
    check(loadManifest(manifestPath).files.size() == 1,
          "failed purge keeps the entry for a later retry");

    fs::remove_all(root);
}

void testStaleBackupReportsActionableFailure() {
    const auto root = makeRoot("stale-backup");
    const auto staging = root / "staging";
    const auto target = root / "target";
    const auto manifestPath = root / "manifest.json";
    const auto mod = staging / "ModA";
    writeFile(mod / "fomod" / "ModuleConfig.xml", "mod-config");
    writeFile(target / "fomod" / "moduleconfig.xml", "lowercase-file");
    fs::create_hard_link(target / "fomod" / "moduleconfig.xml",
                         target / "fomod" / "ModuleConfig.xml.vortex_backup");
    writeFile(target / "fomod" / "ModuleConfig.xml", "current-game-file");

    const std::vector<ModSpec> mods = {{"ModA", "ModA", "", {}}};
    const PlanOptions planOptions;
    IndexStore index(staging);
    auto plan = planDeployment(staging, target, mods, Manifest{}, planOptions, index);

    Journal journal(root / "journal.jsonl", platform::operation_id());
    ExecutorContext context;
    context.stagingPath = staging;
    context.targetPath = target;
    context.manifestPath = manifestPath;
    context.instance = "test";

    const auto report = applyDeployment(plan, context, journal);
    check(report.failures.size() == 1, "stale backup must fail the file");
    check(report.failures[0].message.find(".vortex_backup") != std::string::npos,
          "failure must explain the backup conflict");
    check(readFile(target / "fomod" / "ModuleConfig.xml") == "current-game-file",
          "existing file must be left untouched");

    fs::remove_all(root);
}

void testFileErrorsBecomeFailures() {
    const auto root = makeRoot("failure");
    const auto staging = root / "staging";
    const auto target = root / "target";
    const auto manifestPath = root / "manifest.json";
    const auto mod = staging / "ModA";
    writeFile(mod / "missing.txt", "will be removed");

    const std::vector<ModSpec> mods = {{"ModA", "ModA", "", {}}};
    const PlanOptions planOptions;
    IndexStore index(staging);
    auto plan = planDeployment(staging, target, mods, Manifest{}, planOptions, index);
    fs::remove(mod / "missing.txt");

    Journal journal(root / "journal.jsonl", platform::operation_id());
    ExecutorContext context;
    context.stagingPath = staging;
    context.targetPath = target;
    context.manifestPath = manifestPath;
    context.instance = "test";

    const auto report = applyDeployment(plan, context, journal);
    check(!report.failures.empty(), "missing source should produce a failure");
    check(readFile(manifestPath).find("\"files\": []") != std::string::npos,
          "failed file should not be recorded in the manifest");

    fs::remove_all(root);
}

void testStagingStatus() {
    const auto root = makeRoot("status");
    const auto staging = root / "staging";
    writeFile(staging / "ModA" / "a.txt", "a");
    fs::create_directories(staging / "EmptyMod");

    const std::vector<vortex::core::ModStatusQuery> queries = {
        {"a", "ModA"},
        {"empty", "EmptyMod"},
        {"missing", "MissingMod"},
        {"traversal", "../escape"},
    };
    const auto results = vortex::core::inspectMods(staging, queries);
    check(results.size() == 4, "status result count");
    check(results[0].status == vortex::core::ModStatus::Ok, "installed mod is ok");
    check(results[0].entries == 1, "installed mod entry count");
    check(results[1].status == vortex::core::ModStatus::Empty, "empty mod is empty");
    check(results[2].status == vortex::core::ModStatus::Missing, "missing mod is missing");
    check(results[3].status == vortex::core::ModStatus::Error, "traversal is rejected");

    fs::remove_all(root);
}

void testInstallApply() {
    const auto root = makeRoot("install");
    const auto temp = root / "temp";
    const auto destination = root / "destination";
    writeFile(temp / "a.txt", "a");
    writeFile(temp / "nested" / "b.txt", "b");

    std::vector<vortex::core::InstallInstruction> instructions;
    instructions.push_back(
        {vortex::core::InstallInstructionType::Mkdir, "", "empty", "", false});
    instructions.push_back(
        {vortex::core::InstallInstructionType::Hardlink, "a.txt", "a.txt", "", false});
    instructions.push_back({vortex::core::InstallInstructionType::Copy, "nested/b.txt",
                            "nested/b.txt", "", false});
    instructions.push_back(
        {vortex::core::InstallInstructionType::Write, "", "generated.txt", "hello", false});
    instructions.push_back(
        {vortex::core::InstallInstructionType::Copy, "missing.txt", "missing.txt", "", true});

    vortex::core::InstallRequest request;
    request.tempPath = temp;
    request.destinationPath = destination;
    request.instructions = instructions;
    request.operationId = "test-install";
    const auto report = vortex::core::applyInstall(request);

    check(report.failures.empty(), "install should not fail");
    check(report.directories == 1, "install directory count");
    check(report.linked == 1, "install link count");
    check(report.copied == 1, "install copy count");
    check(report.written == 1, "install write count");
    check(readFile(destination / "a.txt") == "a", "linked content");
    check(readFile(destination / "nested" / "b.txt") == "b", "copied content");
    check(readFile(destination / "generated.txt") == "hello", "generated content");
    check(!fs::exists(destination / "missing.txt"), "optional missing source is skipped");

    fs::remove_all(root);
}

void testInstallFailuresAndJournal() {
    const auto root = makeRoot("install-failure");
    const auto temp = root / "temp";
    const auto destination = root / "destination";
    writeFile(temp / "a.txt", "a");
    writeFile(root / "outside.txt", "outside");

    std::vector<vortex::core::InstallInstruction> instructions;
    instructions.push_back(
        {vortex::core::InstallInstructionType::Copy, "missing.txt", "missing.txt", "", false});
    instructions.push_back({vortex::core::InstallInstructionType::Copy, "../outside.txt",
                            "escape.txt", "", false});
    instructions.push_back(
        {vortex::core::InstallInstructionType::Copy, "a.txt", "../escape.txt", "", false});

    vortex::core::InstallRequest request;
    request.tempPath = temp;
    request.destinationPath = destination;
    request.instructions = instructions;
    request.journalPath = root / "journal.jsonl";
    request.operationId = "test-failure";
    const auto report = vortex::core::applyInstall(request);

    check(report.failures.size() == 3, "all invalid instructions must fail");
    check(!fs::exists(destination / "escape.txt"), "destination traversal must not write");
    check(!fs::exists(root / "escape.txt"), "source traversal must not be read");

    const auto journal = readFile(root / "journal.jsonl");
    check(journal.find("\"install.begin\"") != std::string::npos, "journal begin event");
    check(journal.find("\"install.complete\"") != std::string::npos, "journal complete event");
    check(journal.find("\"install.failure\"") != std::string::npos, "journal failure event");

    fs::remove_all(root);
}

} // namespace

int main() {
    try {
        testManifestRoundtrip();
        testIndexCachesAndInvalidates();
        testPlannerDiff();
        testDeployAndPurge();
        testPurgeKeepsFailedEntries();
        testStaleBackupReportsActionableFailure();
        testFileErrorsBecomeFailures();
        testStagingStatus();
        testInstallApply();
        testInstallFailuresAndJournal();
        std::cout << "all core and deployment tests passed\n";
        return 0;
    } catch (const std::exception& err) {
        std::cerr << "deployment tests failed: " << err.what() << "\n";
        return 1;
    }
}
