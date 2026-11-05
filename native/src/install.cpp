#include "vortex/core/install.hpp"

#include "vortex/deployment/journal.hpp"
#include "vortex/deployment/platform.hpp"

#include <algorithm>
#include <atomic>
#include <fstream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <system_error>
#include <thread>

namespace fs = std::filesystem;
using vortex::deployment::Journal;
using vortex::deployment::platform::atomic_replace;
using vortex::deployment::platform::operation_id;

namespace vortex::core {

namespace {

bool isSafeRelative(const fs::path& path) {
    if (path.empty() || path.is_absolute() || path.has_root_name() ||
        path.has_root_directory()) {
        return false;
    }
    for (const auto& part : path) {
        if (part == "..") {
            return false;
        }
    }
    return true;
}

bool resolveInside(const fs::path& root, const std::string& relative, fs::path& result,
                   std::string& error) {
    const fs::path child(relative);
    if (!isSafeRelative(child)) {
        error = "path must be relative and stay inside its root: " + relative;
        return false;
    }
    result = root / child;
    return true;
}

void addFailure(std::vector<InstallFailure>& failures, std::mutex& mutex,
                const InstallInstruction& instruction, std::string message) {
    std::lock_guard<std::mutex> lock(mutex);
    failures.push_back({instruction.source, instruction.destination, std::move(message)});
}

bool isMissing(const std::error_code& ec) {
    return ec == std::errc::no_such_file_or_directory || ec == std::errc::not_a_directory;
}

void applyFileInstruction(const InstallRequest& request, const InstallInstruction& instruction,
                          const std::string& operation, std::atomic<std::size_t>& linked,
                          std::atomic<std::size_t>& copied, std::atomic<std::size_t>& written,
                          std::vector<InstallFailure>& failures, std::mutex& failuresMutex) {
    std::string error;
    fs::path destination;
    if (!resolveInside(request.destinationPath, instruction.destination, destination, error)) {
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
    }

    if (instruction.type == InstallInstructionType::Write) {
        const auto temporary =
            fs::path(destination.native() + fs::path(".tmp-" + operation).native());
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
    if (!resolveInside(request.tempPath, instruction.source, source, error)) {
        addFailure(failures, failuresMutex, instruction, std::move(error));
        return;
    }

    const auto sourceStatus = fs::status(source, ec);
    if (ec || !fs::is_regular_file(sourceStatus)) {
        if (instruction.optional && (ec && isMissing(ec))) {
            return;
        }
        addFailure(failures, failuresMutex, instruction,
                   ec ? "source is unavailable: " + ec.message() : "source is not a file");
        return;
    }

    // A retry may find a previous destination; remove only a non-directory so
    // we never delete a directory by accident.
    const auto destinationStatus = fs::status(destination, ec);
    if (!ec && fs::exists(destinationStatus)) {
        if (fs::is_directory(destinationStatus)) {
            addFailure(failures, failuresMutex, instruction,
                       "destination is a directory");
            return;
        }
        fs::remove(destination, ec);
        if (ec) {
            addFailure(failures, failuresMutex, instruction,
                       "remove existing destination: " + ec.message());
            return;
        }
    }

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

    std::unique_ptr<Journal> journal;
    if (!request.journalPath.empty()) {
        journal = std::make_unique<Journal>(request.journalPath, operation);
        journal->event("install.begin",
                       {{"instructions", request.instructions.size()},
                        {"temp", request.tempPath.string()},
                        {"destination", request.destinationPath.string()}});
    }

    std::vector<const InstallInstruction*> jobs;
    jobs.reserve(request.instructions.size());
    for (const auto& instruction : request.instructions) {
        if (instruction.type == InstallInstructionType::Mkdir) {
            std::string error;
            fs::path destination;
            if (!resolveInside(request.destinationPath, instruction.destination, destination,
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
        jobs.push_back(&instruction);
    }

    const auto hardware = std::thread::hardware_concurrency();
    const auto requested = request.concurrency == 0 ? std::size_t{1} : request.concurrency;
    const auto workers = std::clamp<std::size_t>(
        std::min<std::size_t>(requested, hardware == 0 ? 1 : hardware), 1, 8);
    const auto workerCount = std::min<std::size_t>(workers, jobs.size());
    std::atomic<std::size_t> next{0};

    std::vector<std::thread> threads;
    threads.reserve(workerCount);
    for (std::size_t worker = 0; worker < workerCount; ++worker) {
        threads.emplace_back([&] {
            for (;;) {
                const auto index = next.fetch_add(1, std::memory_order_relaxed);
                if (index >= jobs.size()) {
                    return;
                }
                applyFileInstruction(request, *jobs[index], operation, linked, copied, written,
                                     failures, failuresMutex);
            }
        });
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
        for (const auto& failure : report.failures) {
            journal->event("install.failure", {{"source", failure.source},
                                               {"destination", failure.destination},
                                               {"message", failure.message}});
        }
        journal->event(
            "install.complete",
            {{"directories", report.directories},
             {"linked", report.linked},
             {"copied", report.copied},
             {"written", report.written},
             {"failures", report.failures.size()}},
            true);
    }

    return report;
}

} // namespace vortex::core
