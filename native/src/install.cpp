#include "vortex/core/install.hpp"

#include "vortex/core/path_safety.hpp"
#include "vortex/deployment/journal.hpp"
#include "vortex/deployment/platform.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <system_error>
#include <thread>

namespace fs = std::filesystem;
using vortex::deployment::Journal;
using vortex::deployment::platform::atomic_replace;
using vortex::deployment::platform::is_case_insensitive;
using vortex::deployment::platform::normalized_key;
using vortex::deployment::platform::operation_id;

namespace vortex::core {

namespace {

bool isMissing(const std::error_code& ec) {
    return ec == std::errc::no_such_file_or_directory || ec == std::errc::not_a_directory;
}

void addFailure(std::vector<InstallFailure>& failures, std::mutex& mutex,
                const InstallInstruction& instruction, std::string message) {
    std::lock_guard<std::mutex> lock(mutex);
    failures.push_back({instruction.source, instruction.destination, std::move(message)});
}

void applyFileInstruction(const InstallRequest& request, const InstallInstruction& instruction,
                          const std::string& operation, std::size_t sequence,
                          bool caseInsensitive, std::atomic<std::size_t>& linked,
                          std::atomic<std::size_t>& copied, std::atomic<std::size_t>& written,
                          std::vector<InstallFailure>& failures, std::mutex& failuresMutex) {
    std::string error;
    fs::path destination;
    if (!resolveNoSymlinks(request.destinationPath, instruction.destination, destination, error)) {
        addFailure(failures, failuresMutex, instruction, std::move(error));
        return;
    }

    std::error_code ec;
    if (!destination.parent_path().empty()) {
        fs::create_directories(destination.parent_path(), ec);
        if (ec) {
            addFailure(failures, failuresMutex, instruction,
                       "create destination directory: " + ec.message());
            return;
        }
        // Re-check after creating parents: a concurrent actor must not be able
        // to turn one of them into a symlink between the first check and use.
        if (!resolveNoSymlinks(request.destinationPath, instruction.destination, destination,
                               error)) {
            addFailure(failures, failuresMutex, instruction, std::move(error));
            return;
        }
    }

    if (instruction.type == InstallInstructionType::Write) {
        const auto temporary = fs::path(destination.native() +
                                        fs::path(".tmp-" + operation + "-" +
                                                 std::to_string(sequence))
                                            .native());
        {
            std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
            if (!output) {
                addFailure(failures, failuresMutex, instruction,
                           "failed to create temporary file");
                return;
            }
            output.write(instruction.data.data(),
                         static_cast<std::streamsize>(instruction.data.size()));
            output.flush();
            if (!output) {
                std::error_code cleanup;
                fs::remove(temporary, cleanup);
                addFailure(failures, failuresMutex, instruction,
                           "failed to write temporary file");
                return;
            }
        }
        try {
            atomic_replace(temporary, destination);
            written.fetch_add(1, std::memory_order_relaxed);
        } catch (const std::exception& err) {
            std::error_code cleanup;
            fs::remove(temporary, cleanup);
            addFailure(failures, failuresMutex, instruction,
                       std::string("replace destination: ") + err.what());
        }
        return;
    }

    fs::path source;
    if (!resolveSourceFile(request.tempPath, instruction.source, source, error)) {
        addFailure(failures, failuresMutex, instruction, std::move(error));
        return;
    }

    const auto sourceStatus = fs::symlink_status(source, ec);
    if (ec) {
        if (instruction.optional && isMissing(ec)) {
            return;
        }
        addFailure(failures, failuresMutex, instruction,
                   ec ? "source is unavailable: " + ec.message() : "source is unavailable");
        return;
    }
    if (!fs::exists(sourceStatus)) {
        if (instruction.optional) {
            return;
        }
        addFailure(failures, failuresMutex, instruction, "source does not exist");
        return;
    }
    if (!fs::is_regular_file(sourceStatus)) {
        addFailure(failures, failuresMutex, instruction, "source is not a regular file");
        return;
    }

    // A retry may find a previous destination; remove only a non-directory so
    // we never delete a directory by accident. resolveNoSymlinks already
    // rejected a symlink in this position.
    const auto destinationStatus = fs::symlink_status(destination, ec);
    if (ec && !isMissing(ec)) {
        addFailure(failures, failuresMutex, instruction,
                   "inspect existing destination: " + ec.message());
        return;
    }
    if (!ec && fs::exists(destinationStatus)) {
        if (fs::is_directory(destinationStatus) || fs::is_symlink(destinationStatus)) {
            addFailure(failures, failuresMutex, instruction,
                       "destination is not a regular file");
            return;
        }
        fs::remove(destination, ec);
        if (ec) {
            addFailure(failures, failuresMutex, instruction,
                       "remove existing destination: " + ec.message());
            return;
        }
    }

    (void)caseInsensitive;
    if (instruction.type == InstallInstructionType::Hardlink) {
        fs::create_hard_link(source, destination, ec);
        if (!ec) {
            linked.fetch_add(1, std::memory_order_relaxed);
            return;
        }
    }

    fs::copy_file(source, destination, fs::copy_options::overwrite_existing, ec);
    if (ec) {
        addFailure(failures, failuresMutex, instruction, "copy file: " + ec.message());
        return;
    }
    copied.fetch_add(1, std::memory_order_relaxed);
}

} // namespace

InstallReport applyInstall(const InstallRequest& request) {
    if (request.tempPath.empty() || request.destinationPath.empty()) {
        throw std::runtime_error("install requires tempPath and destinationPath");
    }

    InstallReport report;
    std::atomic<std::size_t> directories{0};
    std::atomic<std::size_t> linked{0};
    std::atomic<std::size_t> copied{0};
    std::atomic<std::size_t> written{0};
    std::vector<InstallFailure> failures;
    std::mutex failuresMutex;
    const auto operation = request.operationId.empty() ? operation_id() : request.operationId;
    const auto caseInsensitive = is_case_insensitive(request.destinationPath);

    std::unique_ptr<Journal> journal;
    if (!request.journalPath.empty()) {
        journal = std::make_unique<Journal>(request.journalPath, operation);
        journal->event("install.begin",
                       {{"instructions", request.instructions.size()},
                        {"temp", request.tempPath.string()},
                        {"destination", request.destinationPath.string()}},
                       true);
    }

    struct InstructionGroup {
        std::string key;
        std::vector<const InstallInstruction*> instructions;
    };
    std::vector<InstructionGroup> groups;
    std::map<std::string, std::size_t> groupByKey;

    for (const auto& instruction : request.instructions) {
        if (instruction.type == InstallInstructionType::Mkdir) {
            std::string error;
            fs::path destination;
            if (!resolveNoSymlinks(request.destinationPath, instruction.destination, destination,
                                   error)) {
                addFailure(failures, failuresMutex, instruction, std::move(error));
                continue;
            }
            std::error_code ec;
            if (fs::create_directories(destination, ec)) {
                directories.fetch_add(1, std::memory_order_relaxed);
            }
            if (ec) {
                addFailure(failures, failuresMutex, instruction,
                           "create directory: " + ec.message());
            }
            continue;
        }

        try {
            const auto key = normalized_key(fs::path(instruction.destination), caseInsensitive);
            auto found = groupByKey.find(key);
            if (found == groupByKey.end()) {
                const auto index = groups.size();
                groupByKey.emplace(key, index);
                groups.push_back({key, {&instruction}});
            } else {
                groups[found->second].instructions.push_back(&instruction);
            }
        } catch (const std::exception& err) {
            addFailure(failures, failuresMutex, instruction,
                       std::string("invalid destination: ") + err.what());
        }
    }

    const auto hardware = std::thread::hardware_concurrency();
    const auto requested = request.concurrency == 0 ? std::size_t{1} : request.concurrency;
    const auto workers = std::clamp<std::size_t>(
        std::min<std::size_t>(requested, hardware == 0 ? 1 : hardware), 1, 8);
    const auto workerCount = std::min<std::size_t>(workers, groups.size());
    std::atomic<std::size_t> next{0};
    std::atomic<std::size_t> sequence{0};

    std::vector<std::thread> threads;
    threads.reserve(workerCount);
    try {
        for (std::size_t worker = 0; worker < workerCount; ++worker) {
            threads.emplace_back([&] {
                for (;;) {
                    const auto index = next.fetch_add(1, std::memory_order_relaxed);
                    if (index >= groups.size()) {
                        return;
                    }
                    for (const auto* instruction : groups[index].instructions) {
                        try {
                            applyFileInstruction(
                                request, *instruction, operation,
                                sequence.fetch_add(1, std::memory_order_relaxed), caseInsensitive,
                                linked, copied, written, failures, failuresMutex);
                        } catch (const std::exception& err) {
                            addFailure(failures, failuresMutex, *instruction,
                                       std::string("internal error: ") + err.what());
                        } catch (...) {
                            addFailure(failures, failuresMutex, *instruction,
                                       "internal error: unknown exception");
                        }
                    }
                }
            });
        }
    } catch (...) {
        for (auto& thread : threads) {
            thread.join();
        }
        throw;
    }
    for (auto& thread : threads) {
        thread.join();
    }

    report.directories = directories.load(std::memory_order_relaxed);
    report.linked = linked.load(std::memory_order_relaxed);
    report.copied = copied.load(std::memory_order_relaxed);
    report.written = written.load(std::memory_order_relaxed);
    report.failures = std::move(failures);

    if (journal) {
        try {
            for (const auto& failure : report.failures) {
                journal->event("install.failure", {{"source", failure.source},
                                                   {"destination", failure.destination},
                                                   {"message", failure.message}});
            }
            const auto event = report.failures.empty() ? "install.complete" : "install.failed";
            journal->event(event,
                           {{"directories", report.directories},
                            {"linked", report.linked},
                            {"copied", report.copied},
                            {"written", report.written},
                            {"failures", report.failures.size()}},
                           true);
        } catch (const std::exception& err) {
            // Files are already applied. Do not turn a successful install into
            // an error just because the journal could not be finalised.
            std::fprintf(stderr, "install journal finalisation failed: %s\n", err.what());
        }
    }

    return report;
}

} // namespace vortex::core
