#include "vortex/deployment/journal.hpp"

#include "vortex/deployment/platform.hpp"

#include <cstdio>
#include <filesystem>
#include <stdexcept>
#include <system_error>

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace vortex::deployment {

Journal::Journal(fs::path path, std::string operationId)
    : path_(std::move(path)), operationId_(std::move(operationId)) {
    std::error_code ec;
    if (!path_.parent_path().empty()) {
        fs::create_directories(path_.parent_path(), ec);
        if (ec) {
            throw std::system_error(ec, "create journal directory");
        }
    }

    file_ = std::fopen(path_.string().c_str(), "ab");
    if (file_ == nullptr) {
        throw std::runtime_error("failed to open journal: " + path_.string());
    }
}

Journal::~Journal() {
    if (file_ != nullptr) {
        std::fclose(static_cast<std::FILE*>(file_));
        file_ = nullptr;
    }
}

void Journal::event(const std::string& type, const nlohmann::json& details, bool durable) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (file_ == nullptr) {
        throw std::runtime_error("journal is not open");
    }

    nlohmann::json record = details.is_object() ? details : nlohmann::json::object();
    record["ts"] = platform::now_ms();
    record["op"] = operationId_;
    record["event"] = type;

    const auto line = record.dump() + "\n";
    auto* stream = static_cast<std::FILE*>(file_);
    if (std::fwrite(line.data(), 1, line.size(), stream) != line.size()) {
        throw std::runtime_error("failed to write journal: " + path_.string());
    }
    std::fflush(stream);

    if (durable) {
#ifdef _WIN32
        if (_commit(_fileno(stream)) != 0) {
            throw std::runtime_error("failed to sync journal: " + path_.string());
        }
#else
        if (::fsync(fileno(stream)) != 0) {
            throw std::runtime_error("failed to sync journal: " + path_.string());
        }
#endif
    }
}

} // namespace vortex::deployment
