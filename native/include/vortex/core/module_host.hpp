#pragma once

#include "vortex/core/module_abi.h"

#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace vortex::core {

struct ModuleInfo {
    std::string name;
    std::string version;
    std::filesystem::path path;
};

/**
 * Loads C-ABI modules (`vortex_module_entry`) from shared libraries and
 * dispatches JSON calls to them. Modules are the boundary for domain code that
 * must stay outside the core or be supplied by a first-party/community plugin.
 */
class ModuleHost {
public:
    ModuleHost() = default;
    ~ModuleHost();

    ModuleHost(const ModuleHost&) = delete;
    ModuleHost& operator=(const ModuleHost&) = delete;

    ModuleInfo load(const std::filesystem::path& path);
    std::string call(const std::string& name, const std::string& method,
                     const std::string& params) const;
    std::vector<ModuleInfo> list() const;
    bool contains(const std::string& name) const;

private:
    struct Loaded {
        void* handle = nullptr;
        const vortex_module_v1* module = nullptr;
        std::filesystem::path path;
    };

    std::map<std::string, Loaded> modules_;
};

} // namespace vortex::core
