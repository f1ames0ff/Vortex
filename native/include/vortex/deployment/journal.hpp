#pragma once

#include <nlohmann/json.hpp>

#include <filesystem>
#include <mutex>
#include <string>

namespace vortex::deployment {

class Journal {
public:
    Journal(std::filesystem::path path, std::string operationId);
    ~Journal();

    Journal(const Journal&) = delete;
    Journal& operator=(const Journal&) = delete;

    const std::string& operationId() const { return operationId_; }

    void event(const std::string& type, const nlohmann::json& details = nlohmann::json::object(),
               bool durable = false);

private:
    std::filesystem::path path_;
    std::string operationId_;
    void* file_ = nullptr;
    std::mutex mutex_;
};

} // namespace vortex::deployment
