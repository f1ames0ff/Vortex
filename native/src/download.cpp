#include "vortex/core/download.hpp"

#include "vortex/core/path_safety.hpp"
#include "vortex/core/process.hpp"
#include "vortex/deployment/platform.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <sstream>
#include <stdexcept>
#include <system_error>

namespace fs = std::filesystem;

namespace vortex::core {

namespace {

std::string lowerAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

std::string trim(std::string value) {
    const auto notSpace = [](unsigned char character) { return std::isspace(character) == 0; };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), notSpace));
    value.erase(std::find_if(value.rbegin(), value.rend(), notSpace).base(), value.end());
    return value;
}

bool isHttpUrl(const std::string& url) {
    const auto lowered = lowerAscii(url);
    return lowered.rfind("http://", 0) == 0 || lowered.rfind("https://", 0) == 0;
}

bool hasHeaderInjection(const std::string& value) {
    return value.find('\r') != std::string::npos || value.find('\n') != std::string::npos;
}

fs::path partPathFor(const DownloadRequest& request) {
    return request.directory / fs::path(request.fileName + ".part");
}

fs::path finalPathFor(const DownloadRequest& request) {
    return request.directory / fs::path(request.fileName);
}

fs::path activePathFor(const DownloadRequest& request) {
    return request.atomic ? partPathFor(request) : finalPathFor(request);
}

} // namespace

DownloadQueue::DownloadQueue() : scheduler_(&DownloadQueue::schedule, this) {}

DownloadQueue::~DownloadQueue() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
        for (auto& entry : jobs_) {
            entry.second->cancelRequested = true;
        }
    }
    wakeup_.notify_all();
    if (scheduler_.joinable()) {
        scheduler_.join();
    }
    for (auto& entry : jobs_) {
        if (entry.second->worker.joinable()) {
            entry.second->worker.join();
        }
    }
}

std::uint64_t DownloadQueue::fileSize(const fs::path& path) {
    std::error_code ec;
    const auto size = fs::file_size(path, ec);
    return ec ? 0 : size;
}

std::string DownloadQueue::lastError(const std::string& output) {
    std::istringstream stream(output);
    std::string line;
    std::string last;
    while (std::getline(stream, line)) {
        const auto text = trim(line);
        if (!text.empty()) {
            last = text;
        }
    }
    return last.empty() ? "curl failed" : last;
}

std::string DownloadQueue::add(DownloadRequest request) {
    if (request.directory.empty()) {
        throw std::runtime_error("download.directory is required");
    }
    if (request.urls.empty()) {
        throw std::runtime_error("download.urls must not be empty");
    }
    std::string safetyError;
    if (!isSafeRelative(fs::path(request.fileName), safetyError)) {
        throw std::runtime_error("unsafe download file name: " + safetyError);
    }
    for (const auto& url : request.urls) {
        if (!isHttpUrl(url)) {
            throw std::runtime_error("only http(s) downloads are supported: " + url);
        }
    }
    for (const auto& header : request.headers) {
        if (hasHeaderInjection(header.first) || hasHeaderInjection(header.second)) {
            throw std::runtime_error("download header contains a line break");
        }
    }
    if (request.id.empty()) {
        request.id = "download_" + deployment::platform::operation_id();
    }
    if (request.maxRetries < 0) {
        request.maxRetries = 0;
    }

    std::error_code ec;
    fs::create_directories(request.directory, ec);
    if (ec) {
        throw std::runtime_error("create download directory: " + ec.message());
    }
    const auto target = finalPathFor(request);
    if (!target.parent_path().empty() && target.parent_path() != request.directory) {
        fs::create_directories(target.parent_path(), ec);
        if (ec) {
            throw std::runtime_error("create download directory: " + ec.message());
        }
    }

    auto job = std::make_shared<Job>();
    job->request = std::move(request);
    job->status.id = job->request.id;
    job->status.fileName = job->request.fileName;
    job->status.url = job->request.urls.front();
    job->status.startedAt = deployment::platform::now_ms();
    if (!job->request.atomic) {
        // The main-process downloader owns the destination and decides when to
        // reuse it; a start is a fresh transfer, not a cache hit.
        std::error_code cleanup;
        fs::remove(finalPathFor(job->request), cleanup);
        fs::remove(partPathFor(job->request), cleanup);
    } else if (fileSize(finalPathFor(job->request)) > 0) {
        job->done = true;
        job->joined = true;
        job->status.state = "finished";
        job->status.fromCache = true;
        job->status.bytes = fileSize(finalPathFor(job->request));
        job->status.finishedAt = job->status.startedAt;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (jobs_.find(job->status.id) != jobs_.end()) {
            throw std::runtime_error("duplicate download id: " + job->status.id);
        }
        if (job->request.concurrency > 0) {
            concurrency_ = job->request.concurrency;
        }
        jobs_.emplace(job->status.id, job);
    }
    wakeup_.notify_all();
    return job->status.id;
}

std::optional<DownloadStatus> DownloadQueue::status(const std::string& id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = jobs_.find(id);
    if (found == jobs_.end()) {
        return std::nullopt;
    }
    auto status = found->second->status;
    if (status.state == "downloading" || status.state == "queued" || status.state == "paused") {
        status.bytes = fileSize(activePathFor(found->second->request));
    }
    return status;
}

std::vector<DownloadStatus> DownloadQueue::list() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<DownloadStatus> result;
    result.reserve(jobs_.size());
    for (const auto& entry : jobs_) {
        auto status = entry.second->status;
        if (status.state == "downloading" || status.state == "queued" ||
            status.state == "paused") {
            status.bytes = fileSize(activePathFor(entry.second->request));
        }
        result.push_back(std::move(status));
    }
    return result;
}

void DownloadQueue::pause(const std::string& id) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = jobs_.find(id);
        if (found == jobs_.end()) {
            throw std::runtime_error("unknown download: " + id);
        }
        auto& job = *found->second;
        if (job.status.state == "queued") {
            job.paused = true;
            job.status.state = "paused";
        } else if (job.status.state == "downloading") {
            job.paused = true;
            job.cancelRequested = true;
        }
    }
    wakeup_.notify_all();
}

void DownloadQueue::resume(const std::string& id) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = jobs_.find(id);
        if (found == jobs_.end()) {
            throw std::runtime_error("unknown download: " + id);
        }
        auto& job = *found->second;
        if (job.status.state != "paused" && job.status.state != "failed") {
            return;
        }
        job.paused = false;
        job.cancelled = false;
        job.cancelRequested = false;
        job.status.error.clear();
        job.status.state = "queued";
        job.status.finishedAt = 0;
    }
    wakeup_.notify_all();
}

void DownloadQueue::cancel(const std::string& id) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = jobs_.find(id);
        if (found == jobs_.end()) {
            throw std::runtime_error("unknown download: " + id);
        }
        auto& job = *found->second;
        job.cancelled = true;
        job.cancelRequested = true;
        if (job.status.state == "queued" || job.status.state == "paused") {
            std::error_code cleanup;
            fs::remove(partPathFor(job.request), cleanup);
            job.status.state = "cancelled";
            job.status.finishedAt = deployment::platform::now_ms();
        }
    }
    wakeup_.notify_all();
}

void DownloadQueue::schedule() {
    for (;;) {
        std::vector<std::thread> joinNow;
        std::shared_ptr<Job> start;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            if (stopping_) {
                return;
            }

            for (auto& entry : jobs_) {
                auto& job = *entry.second;
                if (job.done && !job.joined) {
                    job.joined = true;
                    if (job.worker.joinable()) {
                        joinNow.push_back(std::move(job.worker));
                    }
                }
            }

            const auto active = std::count_if(jobs_.begin(), jobs_.end(), [](const auto& entry) {
                return entry.second->status.state == "downloading";
            });
            if (active < concurrency_) {
                for (auto& entry : jobs_) {
                    auto& job = *entry.second;
                    if (job.status.state != "queued") {
                        continue;
                    }
                    job.status.state = "downloading";
                    job.status.finishedAt = 0;
                    start = entry.second;
                    break;
                }
            }

            if (start == nullptr) {
                wakeup_.wait_for(lock, std::chrono::milliseconds(50));
            }
        }

        for (auto& thread : joinNow) {
            if (thread.joinable()) {
                thread.join();
            }
        }

        if (start != nullptr) {
            start->worker = std::thread(&DownloadQueue::run, this, start);
        }
    }
}

void DownloadQueue::run(const std::shared_ptr<Job>& job) {
    auto& request = job->request;
    auto& status = job->status;

    const auto final = finalPathFor(request);
    const auto part = request.atomic ? partPathFor(request) : final;
    std::string error;
    bool success = false;

    for (;;) {
        std::string url;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (job->cancelled || job->paused) {
                break;
            }
            url = request.urls[static_cast<std::size_t>(status.attempts) %
                               request.urls.size()];
            status.url = url;
            status.state = "downloading";
        }

        std::vector<std::string> arguments = {
            "--fail",
            "--location",
            "--silent",
            "--show-error",
            "--retry",
            "0",
            "--output",
            part,
        };
        if (fileSize(part) > 0) {
            arguments.push_back("--continue-at");
            arguments.push_back("-");
        }
        arguments.push_back("--connect-timeout");
        arguments.push_back("30");
        if (!request.userAgent.empty()) {
            arguments.push_back("--user-agent");
            arguments.push_back(request.userAgent);
        }
        if (!request.proxy.empty()) {
            arguments.push_back("--proxy");
            arguments.push_back(request.proxy);
        }
        arguments.push_back("--referer");
        arguments.push_back(request.referer);
        for (const auto& header : request.headers) {
            arguments.push_back("--header");
            arguments.push_back(header.first + ": " + header.second);
        }
        arguments.push_back("--");
        arguments.push_back(url);

        bool cancelled = false;
        try {
            const auto process = runProcess(request.curlPath, arguments, &job->cancelRequested);
            cancelled = process.cancelled;
            success = !process.cancelled && process.exitCode == 0 && fileSize(part) > 0;
            if (!success && !process.cancelled) {
                error = lastError(process.output);
            }
        } catch (const std::exception& err) {
            error = err.what();
        }

        std::lock_guard<std::mutex> lock(mutex_);
        if (job->cancelled || job->paused || cancelled || success) {
            break;
        }
        ++status.attempts;
        status.error = error;
        if (status.attempts > request.maxRetries) {
            status.state = "failed";
            status.finishedAt = deployment::platform::now_ms();
            break;
        }
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        status.bytes = fileSize(part);
        if (job->cancelled) {
            status.state = "cancelled";
            status.finishedAt = deployment::platform::now_ms();
            if (request.atomic) {
                std::error_code cleanup;
                fs::remove(part, cleanup);
            }
        } else if (job->paused) {
            status.state = "paused";
        } else if (success) {
            status.state = "finished";
            status.error.clear();
            if (part != final) {
                std::error_code ec;
                fs::remove(final, ec);
                ec.clear();
                fs::rename(part, final, ec);
                if (ec) {
                    status.state = "failed";
                    status.error = "finalise download: " + ec.message();
                }
            }
            status.bytes = fileSize(final);
            status.finishedAt = deployment::platform::now_ms();
        } else if (status.state != "failed") {
            status.state = "failed";
            status.error = error.empty() ? "download failed" : error;
            status.finishedAt = deployment::platform::now_ms();
        }
    }
    job->done = true;
    wakeup_.notify_all();
}

} // namespace vortex::core
