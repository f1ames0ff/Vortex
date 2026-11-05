#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace vortex::core {

struct DownloadRequest {
    std::string id;
    std::filesystem::path directory;
    std::string fileName;
    std::vector<std::string> urls;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string referer;
    std::string userAgent;
    std::string proxy;
    /** When false, write directly to the final path (partial files resume). */
    bool atomic = true;
    std::filesystem::path curlPath = "curl";
    int maxRetries = 3;
    int concurrency = 3;
};

struct DownloadStatus {
    std::string id;
    std::string state = "queued";
    std::string fileName;
    std::string url;
    std::uint64_t bytes = 0;
    int attempts = 0;
    bool fromCache = false;
    std::string error;
    std::int64_t startedAt = 0;
    std::int64_t finishedAt = 0;
};

/**
 * Core-owned download queue. Transfers are executed by the platform curl
 * binary (the same external-tool approach used for 7-Zip): the core owns
 * ordering, concurrency, retries, pause/resume/cancel and the final atomic
 * rename, so no renderer work is needed while a download is in flight.
 */
class DownloadQueue {
public:
    DownloadQueue();
    ~DownloadQueue();

    DownloadQueue(const DownloadQueue&) = delete;
    DownloadQueue& operator=(const DownloadQueue&) = delete;

    std::string add(DownloadRequest request);
    std::optional<DownloadStatus> status(const std::string& id) const;
    std::vector<DownloadStatus> list() const;

    void pause(const std::string& id);
    void resume(const std::string& id);
    void cancel(const std::string& id);

private:
    struct Job {
        DownloadRequest request;
        DownloadStatus status;
        std::atomic<bool> cancelRequested{false};
        bool paused = false;
        bool cancelled = false;
        std::atomic<bool> done{false};
        bool joined = false;
        std::thread worker;
    };

    void schedule();
    void run(const std::shared_ptr<Job>& job);
    static std::uint64_t fileSize(const std::filesystem::path& path);
    static std::string lastError(const std::string& output);

    mutable std::mutex mutex_;
    std::condition_variable wakeup_;
    std::map<std::string, std::shared_ptr<Job>> jobs_;
    bool stopping_ = false;
    int concurrency_ = 3;
    std::thread scheduler_;
};

} // namespace vortex::core
