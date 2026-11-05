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
#include "vortex/core/process.hpp"
#include "vortex/core/status.hpp"
#include "vortex/game/descriptor.hpp"
#include "vortex/game/toml.hpp"

#include <algorithm>
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

void expectTomlError(const std::string& text, std::size_t line, const std::string& needle) {
    try {
        (void)vortex::game::parseToml(text, "malformed.toml");
    } catch (const vortex::game::TomlError& err) {
        check(err.line() == line, "TOML error should report the right line");
        check(std::string(err.what()).find(needle) != std::string::npos,
              "TOML error should mention the problem");
        return;
    }
    throw std::runtime_error("expected malformed TOML to be rejected");
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

void testSymlinkDeployAndPurge() {
#ifndef _WIN32
    const auto root = makeRoot("symlink-deploy");
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
    context.deploymentMethod = "symlink";

    const auto report = applyDeployment(plan, context, journal);
    check(report.failures.empty(), "symlink deploy should not fail");
    check(fs::is_symlink(target / "a.txt"), "deployed file is a symlink");
    check(platform::is_symlink_to(mod / "a.txt", target / "a.txt"), "symlink points at staging");
    check(readFile(target / "a.txt") == "mod-a", "symlink content");

    context.previous = loadManifest(manifestPath);
    Journal purgeJournal(root / "purge.jsonl", platform::operation_id());
    const auto purgeReport = purgeDeployment(context.previous, context, purgeJournal);
    check(purgeReport.failures.empty(), "symlink purge should not fail");
    check(!fs::is_symlink(target / "a.txt"), "purge removes the symlink");
    check(readFile(target / "a.txt") == "game-a", "symlink purge restores the game file");

    fs::remove_all(root);
#endif
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
    request.journalPath = root / "success-journal.jsonl";
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
    check(readFile(root / "success-journal.jsonl").find("\"install.complete\"") !=
              std::string::npos,
          "successful install journal event");

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
    check(journal.find("\"install.failed\"") != std::string::npos, "journal failed event");
    check(journal.find("\"install.failure\"") != std::string::npos, "journal failure event");

    fs::remove_all(root);
}

void testInstallRejectsSymlinkEscapes() {
#ifndef _WIN32
    const auto root = makeRoot("install-symlink");
    const auto temp = root / "temp";
    const auto destination = root / "destination";
    const auto outside = root / "outside";
    writeFile(temp / "a.txt", "inside");
    writeFile(outside / "secret.txt", "secret");
    fs::create_directories(destination);
    fs::create_symlink(outside / "secret.txt", temp / "source-link.txt");
    fs::create_symlink(outside / "secret.txt", destination / "destination-link.txt");
    fs::create_symlink(outside, destination / "sub");

    std::vector<vortex::core::InstallInstruction> instructions;
    instructions.push_back({vortex::core::InstallInstructionType::Copy, "source-link.txt",
                            "from-source.txt", "", false});
    instructions.push_back({vortex::core::InstallInstructionType::Copy, "a.txt",
                            "destination-link.txt", "", false});
    instructions.push_back({vortex::core::InstallInstructionType::Copy, "a.txt",
                            "sub/escape.txt", "", false});

    vortex::core::InstallRequest request;
    request.tempPath = temp;
    request.destinationPath = destination;
    request.instructions = instructions;
    request.operationId = "test-symlink";
    const auto report = vortex::core::applyInstall(request);

    check(report.failures.size() == 3, "all symlink escapes must fail");
    check(!fs::exists(destination / "from-source.txt"), "source symlink must not be copied");
    check(!fs::exists(outside / "escape.txt"), "destination parent symlink must not be followed");
    check(readFile(outside / "secret.txt") == "secret", "outside file must stay untouched");

    fs::remove_all(root);
#endif
}

void testHashVectors() {
    const auto root = makeRoot("hash");
    const auto empty = root / "empty.txt";
    const auto hello = root / "hello.txt";
    writeFile(empty, "");
    writeFile(hello, "hello");

    const auto md5 = vortex::core::hashFiles({empty.string(), hello.string()},
                                             vortex::core::HashAlgorithm::Md5);
    check(md5.failures.empty(), "md5 should not fail");
    check(md5.results[0].hash == "d41d8cd98f00b204e9800998ecf8427e", "md5 empty vector");
    check(md5.results[1].hash == "5d41402abc4b2a76b9719d911017c592", "md5 hello vector");
    check(md5.results[1].bytes == 5, "md5 byte count");

    const auto xxh = vortex::core::hashFiles({empty.string(), hello.string()},
                                             vortex::core::HashAlgorithm::Xxh64);
    check(xxh.failures.empty(), "xxh64 should not fail");
    check(xxh.results[0].hash == "70bbN1HY6Zk=", "xxh64 empty vector");
    check(xxh.results[1].hash == "JseCfYifbaM=", "xxh64 hello vector");

    const auto missing = vortex::core::hashFiles({(root / "missing.txt").string()},
                                                 vortex::core::HashAlgorithm::Md5);
    check(missing.failures.size() == 1, "missing hash failure");

    fs::remove_all(root);
}

void testArchiveListAndExtract() {
#ifdef VORTEX_7Z_PATH
    const auto root = makeRoot("archive");
    const auto source = root / "source";
    const auto archive = root / "sample.zip";
    writeFile(source / "a.txt", "hello");
    writeFile(source / "nested" / "b.txt", "world!");

    const auto previous = fs::current_path();
    fs::current_path(source);
    vortex::core::ProcessResult created;
    try {
        created = vortex::core::runProcess(
            fs::path(VORTEX_7Z_PATH),
            {"a", "-tzip", archive.string(), "a.txt", "nested/b.txt"});
    } catch (...) {
        fs::current_path(previous);
        throw;
    }
    fs::current_path(previous);
    check(created.exitCode == 0, "create archive");

    const auto listing =
        vortex::core::listArchive(archive, fs::path(VORTEX_7Z_PATH));
    check(listing.code == 0, "list archive");
    check(listing.errors.empty(), "list archive errors");
    const auto hasEntry = [&](const std::string& path) {
        return std::any_of(listing.entries.begin(), listing.entries.end(),
                           [&](const vortex::core::ArchiveEntry& entry) {
                               return entry.path == path;
                           });
    };
    check(hasEntry("a.txt"), "archive lists a.txt");
    check(hasEntry("nested/b.txt"), "archive lists nested/b.txt");

    const auto destination = root / "out";
    const auto extraction =
        vortex::core::extractArchive(archive, destination, fs::path(VORTEX_7Z_PATH));
    check(extraction.code == 0, "extract archive");
    check(extraction.errors.empty(), "extract archive errors");
    check(readFile(destination / "a.txt") == "hello", "extracted a.txt");
    check(readFile(destination / "nested" / "b.txt") == "world!", "extracted nested/b.txt");

    fs::remove_all(root);
#endif
}

void testCollectionJobQueue() {
    vortex::core::CollectionJob job;
    job.collectionId = "collection";
    job.gameId = "skyrimse";
    job.concurrency = 1;
    job.members = {
        {0, "requires_a", "requires", 1, false, "pending", std::nullopt, std::nullopt, "", 0},
        {1, "requires_b", "requires", 1, false, "pending", std::nullopt, std::nullopt, "", 0},
        {2, "requires_c", "requires", 2, false, "pending", std::nullopt, std::nullopt, "", 0},
        {3, "recommends_d", "recommends", 666, false, "pending", std::nullopt, std::nullopt, "", 0},
        {4, "requires_ignored", "requires", 1, true, "ignored", std::nullopt, std::nullopt, "", 0},
    };

    vortex::core::CollectionJobs jobs;
    const auto id = jobs.start(job);
    check(!id.empty(), "collection job id");

    auto first = jobs.next(id);
    check(first.size() == 1 && first[0].ruleId == "requires_a", "first phase member");
    jobs.update(id, {"requires_a", "installed", "mod-a", std::nullopt, "", false});

    auto second = jobs.next(id);
    check(second.size() == 1 && second[0].ruleId == "requires_b", "second phase member");
    jobs.update(id, {"requires_b", "installed", "mod-b", std::nullopt, "", false});

    auto third = jobs.next(id);
    check(third.size() == 1 && third[0].ruleId == "requires_c", "next phase member");
    jobs.update(id, {"requires_c", "installed", "mod-c", std::nullopt, "", false});

    auto fourth = jobs.next(id);
    check(fourth.size() == 1 && fourth[0].ruleId == "recommends_d", "optional phase last");
    jobs.update(id, {"recommends_d", "installed", "mod-d", std::nullopt, "", false});

    const auto* finished = jobs.find(id);
    check(finished != nullptr, "collection job exists");
    const auto status = vortex::core::CollectionJobs::status(*finished);
    check(status["complete"].get<bool>(), "collection job complete");
    check(status["counts"]["installed"].get<std::size_t>() == 4, "installed count");
    check(status["counts"]["ignored"].get<std::size_t>() == 1, "ignored count");

    // Pause and cancel stop scheduling.
    jobs.pause(id);
    check(jobs.next(id).empty(), "paused job schedules nothing");
    jobs.cancel(id);
    check(jobs.next(id).empty(), "cancelled job schedules nothing");
}

void testCollectionJobRetryAndStall() {
    vortex::core::CollectionJob job;
    job.concurrency = 1;
    job.maxRetries = 2;
    job.stallMs = 0;
    job.members = {{0, "requires_a", "requires", 1, false, "pending", std::nullopt, std::nullopt, "", 0}};
    vortex::core::CollectionJobs jobs;
    const auto id = jobs.start(job);

    check(jobs.next(id).size() == 1, "retry first attempt");
    jobs.update(id, {"requires_a", "failed", std::nullopt, std::nullopt, "transient", true});
    auto status = vortex::core::CollectionJobs::status(*jobs.find(id));
    check(status["members"][0]["status"] == "pending", "failed member requeued");
    check(status["members"][0]["attempts"] == 1, "retry attempt count");

    check(jobs.next(id).size() == 1, "retry second attempt");
    jobs.update(id, {"requires_a", "failed", std::nullopt, std::nullopt, "permanent", false});
    status = vortex::core::CollectionJobs::status(*jobs.find(id));
    check(status["members"][0]["status"] == "failed", "failed member terminal");
    check(status["complete"].get<bool>(), "failed member completes the phase");

    // A fresh job with no progress is reported as stalled once the threshold passes.
    vortex::core::CollectionJob stalled;
    stalled.stallMs = 0;
    stalled.members = {{0, "requires_a", "requires", 1, false, "pending", std::nullopt, std::nullopt, "", 0}};
    const auto stalledId = jobs.start(stalled);
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    const auto stalledStatus = vortex::core::CollectionJobs::status(*jobs.find(stalledId));
    check(stalledStatus["stalled"].get<bool>(), "stalled job reported");
}

void testPluginScan() {
    const auto root = makeRoot("plugins-scan");
    writeFile(root / "Skyrim.esm", "esm");
    writeFile(root / "Mod.ESP", "esp");
    writeFile(root / "Mod.esl", "esl");
    writeFile(root / "Disabled.esp.ghost", "ghost");
    writeFile(root / "readme.txt", "not a plugin");

    const auto result =
        vortex::core::scanPlugins({{root, {".esm", ".esp", ".esl"}}});
    check(result.directories.size() == 1, "plugin scan directory count");
    check(result.directories[0].error.empty(), "plugin scan error");
    const auto& files = result.directories[0].files;
    check(files.size() == 4, "plugin scan file count");
    const auto findName = [&](const std::string& name) {
        return std::find_if(files.begin(), files.end(), [&](const auto& file) {
            return file.name == name;
        });
    };
    const auto ghost = findName("Disabled.esp.ghost");
    check(ghost != files.end(), "ghost plugin present");
    check(ghost->pluginId == "disabled.esp", "ghost plugin id");
    check(ghost->ghost, "ghost flag");
    const auto mixedCase = findName("Mod.ESP");
    check(mixedCase != files.end(), "case-preserved plugin name");
    check(mixedCase->pluginId == "mod.esp", "lowercase plugin id");

    fs::remove_all(root);
}

void testPluginOrderCasingAndWrite() {
    const auto root = makeRoot("plugin-order");
    const auto proton = root / "compatdata" / "489830" / "pfx" / "drive_c" / "users" /
                        "steamuser" / "AppData" / "Local" / "Skyrim Special Edition";
    writeFile(proton / "Plugins.txt",
              "# Automatically generated by Vortex\r\n*Skyrim.esm\r\nMod.esp\r\n");
    writeFile(proton / "loadorder.txt", "# Automatically generated by Vortex\r\nSkyrim.esm\r\n");

    const auto read = vortex::core::readPluginOrder(proton);
    check(read.error.empty(), "plugin order read error");
    check(read.pluginsFile == "Plugins.txt", "proton plugins casing");
    check(read.plugins.size() == 2, "plugin rows");
    check(read.plugins[0] == "*Skyrim.esm", "plugin row content");
    check(read.loadOrder.size() == 1, "load order rows");

    const auto written = vortex::core::writePluginOrder(
        proton, {"*Skyrim.esm", "Mod.esp"}, {"Skyrim.esm", "Mod.esp"});
    check(written.error.empty(), "plugin order write error");
    check(written.pluginsFile == "Plugins.txt", "writes proton plugins casing");
    const auto pluginsContent = readFile(proton / "Plugins.txt");
    check(pluginsContent.find("# Automatically generated by Vortex") == 0,
          "plugins header");
    check(pluginsContent.find("*Skyrim.esm\r\nMod.esp") != std::string::npos,
          "plugins rows");
    const auto loadOrderContent = readFile(proton / "loadorder.txt");
    check(loadOrderContent.find("Skyrim.esm\r\nMod.esp") != std::string::npos,
          "load order rows");

    const auto plain = root / "plain";
    writeFile(plain / "plugins.txt", "");
    const auto plainWrite = vortex::core::writePluginOrder(plain, {"Mod.esp"}, {"Mod.esp"});
    check(plainWrite.pluginsFile == "plugins.txt", "plain plugins casing");
    check(fs::exists(plain / "plugins.txt"), "plain plugins file");
    check(fs::exists(plain / "loadorder.txt"), "plain load order file");

    fs::remove_all(root);
}

void testDependencyMatching() {
    vortex::core::LookupMod tagged;
    tagged.id = "tagged";
    tagged.referenceTag = "tag-1";
    tagged.game = {"skyrimse"};
    vortex::core::ModReference byTag;
    byTag.tag = "tag-1";
    byTag.gameId = "skyrimse";
    check(vortex::core::matchModId({tagged}, byTag) == "tagged", "tag match");

    vortex::core::LookupMod hashed;
    hashed.id = "hashed";
    hashed.fileMD5 = "md5-1";
    vortex::core::ModReference byMd5;
    byMd5.fileMD5 = "md5-1";
    check(vortex::core::matchModId({hashed}, byMd5) == "hashed", "md5 match");

    vortex::core::LookupMod expression;
    expression.id = "expression";
    expression.fileName = "Cool Mod-123-1-0.7z";
    vortex::core::ModReference byExpression;
    byExpression.fileExpression = "Cool Mod*";
    check(vortex::core::matchModId({expression}, byExpression) == "expression",
          "file expression match");

    vortex::core::LookupMod repo;
    repo.id = "repo";
    repo.source = "nexus";
    repo.modId = "123";
    repo.fileId = "456";
    vortex::core::ModReference byRepo;
    vortex::core::RepoReference repoReference;
    repoReference.repository = "nexus";
    repoReference.modId = "123";
    repoReference.fileId = "456";
    byRepo.repo = repoReference;
    check(vortex::core::matchModId({repo}, byRepo) == "repo", "repo match");

    vortex::core::LookupMod variantA;
    variantA.id = "variant-a";
    variantA.logicalFileName = "Variant Mod";
    variantA.installerChoices = {{"a", "1"}};
    vortex::core::LookupMod variantB;
    variantB.id = "variant-b";
    variantB.logicalFileName = "Variant Mod";
    variantB.installerChoices = {{"a", "2"}};
    vortex::core::ModReference variantRef;
    variantRef.logicalFileName = "Variant Mod";
    vortex::core::RuleInfo spec;
    spec.installerChoices = {{"a", "2"}};
    check(vortex::core::matchModId({variantA, variantB}, variantRef, &spec) == "variant-b",
          "install spec match");

    vortex::core::DownloadInfo download;
    download.id = "download";
    download.state = "finished";
    download.referenceTag = "download-tag";
    vortex::core::ModReference byDownloadTag;
    byDownloadTag.tag = "download-tag";
    check(vortex::core::matchDownloadId({download}, byDownloadTag) == "download",
          "download tag match");

    vortex::core::LookupMod versioned;
    versioned.id = "versioned";
    versioned.logicalFileName = "Versioned Mod";
    versioned.version = "1.5.2";
    vortex::core::ModReference byVersion;
    byVersion.logicalFileName = "Versioned Mod";
    byVersion.versionMatch = "^1.5.0";
    check(vortex::core::matchModId({versioned}, byVersion) == "versioned", "semver match");
}

void testCollectionPlan() {
    vortex::core::LookupMod mod;
    mod.id = "tagged";
    mod.referenceTag = "tag-1";

    vortex::core::RuleInfo required;
    required.index = 0;
    required.type = "requires";
    required.phase = 1;
    required.reference.tag = "tag-1";

    vortex::core::RuleInfo missing;
    missing.index = 1;
    missing.type = "requires";
    missing.phase = 4;
    missing.reference.tag = "missing";

    vortex::core::RuleInfo optional;
    optional.index = 2;
    optional.type = "recommends";
    optional.phase = 666;
    optional.reference.tag = "tag-1";

    vortex::core::RuleInfo ignored;
    ignored.index = 3;
    ignored.type = "requires";
    ignored.phase = 0;
    ignored.ignored = true;
    ignored.reference.tag = "tag-1";

    const auto plan = vortex::core::planCollection({required, missing, optional, ignored}, {mod}, {});
    check(plan.members.size() == 4, "plan member count");
    check(plan.totalRequired == 3, "plan required count");
    check(plan.totalOptional == 1, "plan optional count");
    check(plan.phases == std::vector<int>({0, 1, 4, 666}), "plan phases");
    check(plan.members[0].status == "installed", "installed member status");
    check(plan.members[1].status == "pending", "missing member status");
    check(plan.members[2].phase == 666, "optional phase");
    check(plan.members[3].status == "ignored", "ignored member status");
}

void testInstallDuplicateDestinations() {
    const auto root = makeRoot("install-duplicates");
    const auto temp = root / "temp";
    const auto destination = root / "destination";
    writeFile(temp / "a.txt", "a");

    std::vector<vortex::core::InstallInstruction> instructions;
    instructions.push_back(
        {vortex::core::InstallInstructionType::Write, "", "same.txt", "first", false});
    instructions.push_back(
        {vortex::core::InstallInstructionType::Write, "", "same.txt", "second", false});
    instructions.push_back(
        {vortex::core::InstallInstructionType::Copy, "a.txt", "same.txt", "", false});

    vortex::core::InstallRequest request;
    request.tempPath = temp;
    request.destinationPath = destination;
    request.instructions = instructions;
    request.operationId = "test-duplicates";
    const auto report = vortex::core::applyInstall(request);

    check(report.failures.empty(), "duplicate destinations must not race");
    check(readFile(destination / "same.txt") == "a", "last instruction wins deterministically");

    fs::remove_all(root);
}

void testTomlParsing() {
    const auto document = vortex::game::parseToml(R"toml(
# a comment
title = "vortex" # trailing comment
enabled = true
count = 1_234
negative = -42
literal = 'C:\Games\Vortex'
game.id = "skyrimse"
numbers = [1, 2, 3]
names = [
    "alpha", # inline comment in an array
    "beta",
]
empty = []

[deployment]
targets = ["Data", "Mods"]
pluginPath = "Data"
)toml",
                                                "sample.toml");

    const auto* title = vortex::game::findToml(document, "title");
    check(title != nullptr && title->isString(), "TOML string value");
    check(title->stringValue == "vortex", "TOML string content");

    const auto* enabled = vortex::game::findToml(document, "enabled");
    check(enabled != nullptr && enabled->isBoolean(), "TOML boolean value");
    check(enabled->booleanValue, "TOML boolean content");

    const auto* count = vortex::game::findToml(document, "count");
    check(count != nullptr && count->isInteger(), "TOML integer value");
    check(count->integerValue == 1234, "TOML integer underscores");

    const auto* negative = vortex::game::findToml(document, "negative");
    check(negative != nullptr && negative->integerValue == -42, "TOML negative integer");

    const auto* literal = vortex::game::findToml(document, "literal");
    check(literal != nullptr && literal->stringValue == "C:\\Games\\Vortex",
          "TOML literal string");

    const auto* dotted = vortex::game::findToml(document, "game.id");
    check(dotted != nullptr && dotted->stringValue == "skyrimse", "TOML dotted key");

    const auto* numbers = vortex::game::findToml(document, "numbers");
    check(numbers != nullptr && numbers->isArray(), "TOML integer array");
    check(numbers->arrayValue.size() == 3, "TOML integer array length");
    check(numbers->arrayValue[0].integerValue == 1 &&
              numbers->arrayValue[2].integerValue == 3,
          "TOML integer array content");

    const auto* names = vortex::game::findToml(document, "names");
    check(names != nullptr && names->isArray(), "TOML string array");
    check(names->arrayValue.size() == 2, "TOML string array length");
    check(names->arrayValue[0].stringValue == "alpha" &&
              names->arrayValue[1].stringValue == "beta",
          "TOML string array content");

    const auto* empty = vortex::game::findToml(document, "empty");
    check(empty != nullptr && empty->isArray() && empty->arrayValue.empty(),
          "TOML empty array");

    const auto* targets = vortex::game::findToml(document, "deployment.targets");
    check(targets != nullptr && targets->isArray(), "TOML table array");
    check(targets->arrayValue.size() == 2, "TOML table array length");
    check(targets->arrayValue[1].stringValue == "Mods", "TOML table array content");

    const auto* pluginPath = vortex::game::findToml(document, "deployment.pluginPath");
    check(pluginPath != nullptr && pluginPath->stringValue == "Data",
          "TOML table string");
}

void testGameDescriptorParsing() {
    const auto path = fs::path(VORTEX_TEST_DATA_DIR) / "games" / "skyrimse" / "game.toml";
    const auto descriptor = vortex::game::loadGameDescriptor(path);

    check(descriptor.id == "skyrimse", "descriptor id");
    check(descriptor.name == "Skyrim Special Edition", "descriptor name");
    check(descriptor.nexusDomain == "skyrimspecialedition", "descriptor nexusDomain");
    check(descriptor.steamAppId.has_value() && *descriptor.steamAppId == 489830,
          "descriptor steamAppId");
    check(descriptor.requiredFiles.size() == 1 &&
              descriptor.requiredFiles[0] == "SkyrimSE.exe",
          "descriptor requiredFiles");
    check(descriptor.deploymentTargets.size() == 1 &&
              descriptor.deploymentTargets[0] == "Data",
          "descriptor deploymentTargets");
    check(descriptor.pluginPath.has_value() && *descriptor.pluginPath == "Data",
          "descriptor pluginPath");
    check(descriptor.iniPath.has_value() && *descriptor.iniPath == "SkyrimPrefs.ini",
          "descriptor iniPath");
    check(descriptor.hooks.size() == 2 &&
              descriptor.hooks[0] == "gamebryo-plugin-management",
          "descriptor hooks");
    check(descriptor.sourcePath == path, "descriptor sourcePath");
}

void testMalformedToml() {
    expectTomlError("id = \"unterminated\n", 1, "unterminated");
    expectTomlError("id = 1\nname\n", 2, "expected");
    expectTomlError("values = [1, 2\n", 1, "unterminated array");
    expectTomlError("a = 1\na = 2\n", 2, "duplicate key");
    expectTomlError("a = 1\n[a]\n", 2, "conflicts");

    const auto root = makeRoot("game-malformed");
    writeFile(root / "game.toml", "id = \"broken\n");
    bool threw = false;
    try {
        (void)vortex::game::loadGameDescriptor(root / "game.toml");
    } catch (const vortex::game::TomlError& err) {
        threw = true;
        check(err.line() == 1, "descriptor TOML error line");
        check(std::string(err.what()).find("game.toml:1") != std::string::npos,
              "descriptor TOML error includes the source path");
    }
    check(threw, "malformed descriptor should be rejected");
    fs::remove_all(root);
}

void testMissingGameDescriptors() {
    const auto root = makeRoot("game-missing");
    const auto missingDescriptor = root / "missing" / "game.toml";

    bool descriptorThrew = false;
    try {
        (void)vortex::game::loadGameDescriptor(missingDescriptor);
    } catch (const std::runtime_error& err) {
        descriptorThrew = true;
        check(std::string(err.what()).find("failed to open game descriptor") !=
                  std::string::npos,
              "missing descriptor error");
    }
    check(descriptorThrew, "missing descriptor should be rejected");

    bool registryThrew = false;
    try {
        (void)vortex::game::loadGameDescriptors(root / "missing-games");
    } catch (const std::runtime_error& err) {
        registryThrew = true;
        check(std::string(err.what()).find("does not exist") != std::string::npos,
              "missing registry error");
    }
    check(registryThrew, "missing registry directory should be rejected");
    fs::remove_all(root);
}

void testGameDescriptorValidation() {
    const auto root = makeRoot("game-validation");
    writeFile(root / "game.toml", "id = \"incomplete\"\nname = \"Incomplete\"\n");

    bool threw = false;
    try {
        (void)vortex::game::loadGameDescriptor(root / "game.toml");
    } catch (const std::runtime_error& err) {
        threw = true;
        check(std::string(err.what()).find("nexusDomain") != std::string::npos,
              "missing field error should name the field");
    }
    check(threw, "descriptor without nexusDomain should be rejected");
    fs::remove_all(root);
}

void testGameRegistryOrder() {
    const auto directory = fs::path(VORTEX_TEST_DATA_DIR) / "games";
    const auto descriptors = vortex::game::loadGameDescriptors(directory);

    check(descriptors.size() == 3, "registry should skip directories without game.toml");
    check(descriptors[0].id == "alpha", "registry first descriptor");
    check(descriptors[1].id == "skyrimse", "registry second descriptor");
    check(descriptors[2].id == "zeta", "registry third descriptor");

    check(descriptors[0].requiredFiles.size() == 1 &&
              descriptors[0].requiredFiles[0] == "alpha.exe",
          "registry reads requirements table");
    check(descriptors[0].deploymentTargets.size() == 1 &&
              descriptors[0].deploymentTargets[0] == ".",
          "registry reads deployment table");
    check(descriptors[2].deploymentTargets.size() == 2 &&
              descriptors[2].deploymentTargets[1] == "Mods",
          "registry reads deployment target list");
}

} // namespace

int main() {
    try {
        testManifestRoundtrip();
        testIndexCachesAndInvalidates();
        testPlannerDiff();
        testDeployAndPurge();
        testSymlinkDeployAndPurge();
        testPurgeKeepsFailedEntries();
        testStaleBackupReportsActionableFailure();
        testFileErrorsBecomeFailures();
        testStagingStatus();
        testInstallApply();
        testInstallFailuresAndJournal();
        testInstallRejectsSymlinkEscapes();
        testInstallDuplicateDestinations();
        testHashVectors();
        testArchiveListAndExtract();
        testDependencyMatching();
        testCollectionPlan();
        testPluginScan();
        testPluginOrderCasingAndWrite();
        testCollectionJobQueue();
        testCollectionJobRetryAndStall();
        testTomlParsing();
        testGameDescriptorParsing();
        testMalformedToml();
        testMissingGameDescriptors();
        testGameDescriptorValidation();
        testGameRegistryOrder();
        std::cout << "all core and deployment tests passed\n";
        return 0;
    } catch (const std::exception& err) {
        std::cerr << "deployment tests failed: " << err.what() << "\n";
        return 1;
    }
}
