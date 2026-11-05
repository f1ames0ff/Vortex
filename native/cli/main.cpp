#include "vortex/deployment/executor.hpp"
#include "vortex/deployment/file_index.hpp"
#include "vortex/deployment/journal.hpp"
#include "vortex/deployment/manifest.hpp"
#include "vortex/deployment/platform.hpp"
#include "vortex/deployment/planner.hpp"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace vortex::deployment;

namespace {

struct Options {
    std::string command;
    fs::path stagingPath;
    fs::path targetPath;
    fs::path manifestPath;
    fs::path journalDir;
    std::string instance;
    std::string method = "hardlink";
    std::string gameId;
    std::string outPath;
    bool caseInsensitive = false;
    std::vector<ModSpec> mods;
    std::vector<std::string> blacklist;
};

void usage() {
    std::cerr
        << "usage:\n"
        << "  vortex-deploy plan   --staging DIR --target DIR --manifest FILE [--mod NAME[:TARGET]]...\n"
        << "  vortex-deploy deploy --staging DIR --target DIR --manifest FILE [--mod NAME[:TARGET]]...\n"
        << "  vortex-deploy purge  --staging DIR --target DIR --manifest FILE\n"
        << "  vortex-deploy scan   --staging DIR\n";
}

std::pair<std::string, std::string> splitMod(const std::string& value) {
    const auto separator = value.find(':');
    if (separator == std::string::npos) {
        return {value, {}};
    }
    return {value.substr(0, separator), value.substr(separator + 1)};
}

Options parseArgs(int argc, char** argv) {
    if (argc < 2) {
        throw std::runtime_error("missing command");
    }

    Options options;
    options.command = argv[1];

    for (int i = 2; i < argc; ++i) {
        const std::string arg = argv[i];
        auto value = [&]() -> std::string {
            if (i + 1 >= argc) {
                throw std::runtime_error("missing value for " + arg);
            }
            return argv[++i];
        };

        if (arg == "--staging") {
            options.stagingPath = value();
        } else if (arg == "--target") {
            options.targetPath = value();
        } else if (arg == "--manifest") {
            options.manifestPath = value();
        } else if (arg == "--journal-dir") {
            options.journalDir = value();
        } else if (arg == "--instance") {
            options.instance = value();
        } else if (arg == "--method") {
            options.method = value();
        } else if (arg == "--game") {
            options.gameId = value();
        } else if (arg == "--out") {
            options.outPath = value();
        } else if (arg == "--blacklist") {
            options.blacklist.push_back(value());
        } else if (arg == "--case-insensitive") {
            options.caseInsensitive = true;
        } else if (arg == "--mod") {
            auto [name, target] = splitMod(value());
            if (name.empty()) {
                throw std::runtime_error("empty mod name");
            }
            options.mods.push_back({name, name, target, {}});
        } else if (arg == "--help" || arg == "-h") {
            usage();
            std::exit(0);
        } else {
            throw std::runtime_error("unknown argument: " + arg);
        }
    }

    if (options.command == "scan") {
        if (options.stagingPath.empty()) {
            throw std::runtime_error("scan requires --staging");
        }
        return options;
    }

    if (options.stagingPath.empty() || options.targetPath.empty() || options.manifestPath.empty()) {
        throw std::runtime_error("--staging, --target and --manifest are required");
    }
    if (options.command == "deploy" || options.command == "plan") {
        if (options.mods.empty()) {
            throw std::runtime_error("at least one --mod is required");
        }
    }
    if (options.command != "plan" && options.command != "deploy" &&
        options.command != "purge") {
        throw std::runtime_error("unknown command: " + options.command);
    }

    return options;
}

Manifest loadOrCreate(const Options& options) {
    if (fs::exists(options.manifestPath)) {
        return loadManifest(options.manifestPath);
    }
    Manifest manifest;
    manifest.instance = options.instance;
    manifest.stagingPath = options.stagingPath.string();
    manifest.targetPath = options.targetPath.string();
    return manifest;
}

int runPlan(const Options& options) {
    auto previous = loadOrCreate(options);
    IndexStore index(options.stagingPath);
    PlanOptions planOptions;
    planOptions.caseInsensitive =
        options.caseInsensitive || platform::is_case_insensitive(options.targetPath);
    planOptions.extraBlacklist = options.blacklist;

    const auto plan = planDeployment(options.stagingPath, options.targetPath, options.mods, previous,
                                     planOptions, index);
    index.flush();

    std::cout << "desired=" << plan.desired.size() << " added=" << plan.added.size()
              << " removed=" << plan.removed.size() << " changed=" << plan.changed.size() << "\n";

    if (!options.outPath.empty()) {
        Manifest output = makeEmptyManifest(previous, platform::now_ms());
        output.stagingPath = options.stagingPath.string();
        output.targetPath = options.targetPath.string();
        output.files = plan.desired;
        saveManifestAtomic(options.outPath, output);
    }
    return 0;
}

int runDeploy(const Options& options) {
    auto previous = loadOrCreate(options);
    IndexStore index(options.stagingPath);
    PlanOptions planOptions;
    planOptions.caseInsensitive =
        options.caseInsensitive || platform::is_case_insensitive(options.targetPath);
    planOptions.extraBlacklist = options.blacklist;

    const auto plan = planDeployment(options.stagingPath, options.targetPath, options.mods, previous,
                                     planOptions, index);
    index.flush();

    const auto operation = platform::operation_id();
    const auto journalDir =
        options.journalDir.empty() ? options.stagingPath / ".vortex-journal" : options.journalDir;
    Journal journal(journalDir / (operation + ".jsonl"), operation);

    ExecutorContext context;
    context.stagingPath = options.stagingPath;
    context.targetPath = options.targetPath;
    context.manifestPath = options.manifestPath;
    context.previous = std::move(previous);
    context.instance = options.instance.empty() ? context.previous.instance : options.instance;
    context.deploymentMethod = options.method;
    context.gameId = options.gameId;
    context.caseInsensitive = planOptions.caseInsensitive;

    const auto report = applyDeployment(plan, context, journal);

    std::cout << "linked=" << report.linked << " removed=" << report.removed
              << " backedUp=" << report.backedUp << " restored=" << report.restored
              << " skipped=" << report.skipped << " failures=" << report.failures.size() << "\n";
    for (const auto& failure : report.failures) {
        std::cerr << failure.source << ":" << failure.relPath << ": " << failure.message << "\n";
    }
    return report.failures.empty() ? 0 : 1;
}

int runPurge(const Options& options) {
    auto manifest = loadManifest(options.manifestPath);
    const auto operation = platform::operation_id();
    const auto journalDir =
        options.journalDir.empty() ? options.stagingPath / ".vortex-journal" : options.journalDir;
    Journal journal(journalDir / (operation + ".jsonl"), operation);

    ExecutorContext context;
    context.stagingPath = options.stagingPath;
    context.targetPath = options.targetPath;
    context.manifestPath = options.manifestPath;
    context.previous = manifest;
    context.instance = options.instance.empty() ? manifest.instance : options.instance;
    context.deploymentMethod = options.method;
    context.gameId = options.gameId;
    context.caseInsensitive =
        options.caseInsensitive || platform::is_case_insensitive(options.targetPath);

    const auto report = purgeDeployment(manifest, context, journal);
    std::cout << "removed=" << report.removed << " restored=" << report.restored
              << " failures=" << report.failures.size() << "\n";
    for (const auto& failure : report.failures) {
        std::cerr << failure.source << ":" << failure.relPath << ": " << failure.message << "\n";
    }
    return report.failures.empty() ? 0 : 1;
}

int runScan(const Options& options) {
    IndexStore index(options.stagingPath);
    std::size_t folders = 0;
    std::size_t files = 0;
    for (const auto& entry : fs::directory_iterator(options.stagingPath)) {
        if (!entry.is_directory()) {
            continue;
        }
        const auto& mod = index.get(entry.path());
        ++folders;
        files += mod.files.size();
    }
    index.flush();
    std::cout << "mods=" << folders << " files=" << files << "\n";
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    try {
        const auto options = parseArgs(argc, argv);
        if (options.command == "plan") {
            return runPlan(options);
        }
        if (options.command == "deploy") {
            return runDeploy(options);
        }
        if (options.command == "purge") {
            return runPurge(options);
        }
        if (options.command == "scan") {
            return runScan(options);
        }
        usage();
        return 2;
    } catch (const std::exception& err) {
        std::cerr << "vortex-deploy: " << err.what() << "\n";
        return 2;
    }
}
