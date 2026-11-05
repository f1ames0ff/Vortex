#include "vortex/core/module_host.hpp"

#include <stdexcept>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace fs = std::filesystem;

namespace vortex::core {

namespace {

void* openLibrary(const fs::path& path) {
#ifdef _WIN32
    return reinterpret_cast<void*>(LoadLibraryW(path.c_str()));
#else
    return ::dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
#endif
}

void closeLibrary(void* handle) {
    if (handle == nullptr) {
        return;
    }
#ifdef _WIN32
    FreeLibrary(reinterpret_cast<HMODULE>(handle));
#else
    ::dlclose(handle);
#endif
}

vortex_module_entry_fn resolveEntry(void* handle) {
#ifdef _WIN32
    return reinterpret_cast<vortex_module_entry_fn>(
        GetProcAddress(reinterpret_cast<HMODULE>(handle), VORTEX_MODULE_ENTRY));
#else
    return reinterpret_cast<vortex_module_entry_fn>(::dlsym(handle, VORTEX_MODULE_ENTRY));
#endif
}

} // namespace

ModuleHost::~ModuleHost() {
    for (auto& entry : modules_) {
        closeLibrary(entry.second.handle);
    }
}

ModuleInfo ModuleHost::load(const fs::path& path) {
    if (path.empty()) {
        throw std::runtime_error("module path is required");
    }
    void* handle = openLibrary(path);
    if (handle == nullptr) {
        throw std::runtime_error("failed to load module: " + path.string());
    }

    const auto entry = resolveEntry(handle);
    if (entry == nullptr) {
        closeLibrary(handle);
        throw std::runtime_error("module has no " + std::string(VORTEX_MODULE_ENTRY) + ": " +
                                 path.string());
    }

    const auto* module = entry();
    if (module == nullptr || module->abi_version != VORTEX_MODULE_ABI_VERSION ||
        module->name == nullptr || module->name[0] == '\0' || module->invoke == nullptr ||
        module->free_string == nullptr) {
        closeLibrary(handle);
        throw std::runtime_error("module has an unsupported ABI: " + path.string());
    }

    const std::string name = module->name;
    if (modules_.find(name) != modules_.end()) {
        closeLibrary(handle);
        throw std::runtime_error("module already loaded: " + name);
    }

    modules_.emplace(name, Loaded{handle, module, path});
    return {name, module->version == nullptr ? "" : module->version, path};
}

std::string ModuleHost::call(const std::string& name, const std::string& method,
                             const std::string& params) const {
    const auto found = modules_.find(name);
    if (found == modules_.end()) {
        throw std::runtime_error("unknown module: " + name);
    }

    char* result = nullptr;
    char* error = nullptr;
    const auto status = found->second.module->invoke(method.c_str(), params.c_str(), &result,
                                                     &error);

    std::string resultValue;
    std::string errorValue;
    if (result != nullptr) {
        resultValue = result;
        found->second.module->free_string(result);
    }
    if (error != nullptr) {
        errorValue = error;
        found->second.module->free_string(error);
    }

    if (status != 0 || !errorValue.empty()) {
        throw std::runtime_error(errorValue.empty() ? "module call failed" : errorValue);
    }
    return resultValue;
}

std::vector<ModuleInfo> ModuleHost::list() const {
    std::vector<ModuleInfo> result;
    result.reserve(modules_.size());
    for (const auto& entry : modules_) {
        result.push_back({entry.first,
                          entry.second.module->version == nullptr
                              ? ""
                              : entry.second.module->version,
                          entry.second.path});
    }
    return result;
}

bool ModuleHost::contains(const std::string& name) const {
    return modules_.find(name) != modules_.end();
}

} // namespace vortex::core
