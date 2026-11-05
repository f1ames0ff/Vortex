#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace vortex::core {

struct PluginScanRequest {
    std::filesystem::path path;
    std::vector<std::string> extensions;
};

struct PluginFile {
    std::string name;
    std::string pluginId;
    bool ghost = false;
};

struct PluginScanDirectory {
    std::string path;
    std::vector<PluginFile> files;
    std::string error;
};

struct PluginScanResult {
    std::vector<PluginScanDirectory> directories;
};

struct PluginOrderResult {
    std::string pluginsFile;
    std::string loadOrderFile;
    std::string pluginsHeader;
    std::string loadOrderHeader;
    std::vector<std::string> plugins;
    std::vector<std::string> loadOrder;
    std::string error;
};

PluginScanResult scanPlugins(const std::vector<PluginScanRequest>& requests);

PluginOrderResult readPluginOrder(const std::filesystem::path& directory);

/**
 * Write plugins.txt and loadorder.txt with the Vortex header and CRLF rows.
 * The caller supplies the already-filtered rows; the core owns file naming
 * (Proton `Plugins.txt` casing) and atomic replacement.
 */
PluginOrderResult writePluginOrder(const std::filesystem::path& directory,
                                   const std::vector<std::string>& plugins,
                                   const std::vector<std::string>& loadOrder);

} // namespace vortex::core
