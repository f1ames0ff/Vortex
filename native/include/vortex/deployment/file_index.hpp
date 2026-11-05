#pragma once

#include "vortex/deployment/types.hpp"

#include <filesystem>
#include <map>
#include <string>

namespace vortex::deployment {

class IndexStore {
public:
    explicit IndexStore(std::filesystem::path stagingPath);

    const ModIndex& get(const std::filesystem::path& sourcePath);
    void verifyFiles(const std::filesystem::path& sourcePath,
                     const std::vector<std::string>& relPaths);
    void flush();

private:
    ModIndex scan(const std::filesystem::path& sourcePath, TimeMs rootMtimeMs);
    void load();

    std::filesystem::path stagingPath_;
    std::filesystem::path indexPath_;
    std::map<std::string, ModIndex> entries_;
    bool dirty_ = false;
};

} // namespace vortex::deployment
