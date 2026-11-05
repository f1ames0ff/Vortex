#include "vortex/core/plugins.hpp"

#include "vortex/deployment/platform.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
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

std::string upperFirst(std::string value) {
    if (!value.empty()) {
        value[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(value[0])));
    }
    return value;
}

bool isProtonPrefix(const fs::path& path) {
    const auto generic = path.generic_string();
    return generic.find("/compatdata/") != std::string::npos &&
           generic.find("/pfx/") != std::string::npos;
}

std::string existingFileName(const fs::path& directory, const std::string& fileName) {
    const auto capitalized = upperFirst(fileName);
    std::error_code ec;
    std::vector<std::string> entries;
    for (const auto& entry : fs::directory_iterator(directory, ec)) {
        if (entry.is_regular_file()) {
            entries.push_back(entry.path().filename().string());
        }
    }
    if (std::find(entries.begin(), entries.end(), capitalized) != entries.end()) {
        return capitalized;
    }
    if (std::find(entries.begin(), entries.end(), fileName) != entries.end()) {
        return fileName;
    }
    const auto lowered = lowerAscii(fileName);
    for (const auto& entry : entries) {
        if (lowerAscii(entry) == lowered) {
            return entry;
        }
    }
    if (isProtonPrefix(directory) && fileName == "plugins.txt") {
        return capitalized;
    }
    return fileName;
}

struct OrderedRows {
    std::string header;
    std::vector<std::string> rows;
};

OrderedRows readRows(const fs::path& path) {
    OrderedRows result;
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return result;
    }
    std::string line;
    if (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        result.header = line;
        if (!line.empty() && line[0] != '#') {
            result.rows.push_back(line);
        }
    }
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty() || line[0] == '#') {
            continue;
        }
        result.rows.push_back(std::move(line));
    }
    return result;
}

void writeAtomic(const fs::path& path, const std::string& content) {
    std::error_code ec;
    if (!path.parent_path().empty()) {
        fs::create_directories(path.parent_path(), ec);
        if (ec) {
            throw std::system_error(ec, "create plugin directory");
        }
    }
    const auto temporary = fs::path(path.native() + fs::path(".tmp-" +
                                                             deployment::platform::operation_id())
                                                           .native());
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) {
            throw std::runtime_error("failed to create temporary plugin file: " +
                                     temporary.string());
        }
        output.write(content.data(), static_cast<std::streamsize>(content.size()));
        output.flush();
        if (!output) {
            throw std::runtime_error("failed to write temporary plugin file: " +
                                     temporary.string());
        }
    }
    try {
        deployment::platform::atomic_replace(temporary, path);
    } catch (...) {
        std::error_code cleanup;
        fs::remove(temporary, cleanup);
        throw;
    }
}

std::string joinRows(const std::vector<std::string>& rows) {
    std::string result;
    for (std::size_t index = 0; index < rows.size(); ++index) {
        if (index > 0) {
            result += "\r\n";
        }
        result += rows[index];
    }
    return result;
}

} // namespace

PluginScanResult scanPlugins(const std::vector<PluginScanRequest>& requests) {
    PluginScanResult result;
    result.directories.reserve(requests.size());

    for (const auto& request : requests) {
        PluginScanDirectory directory;
        directory.path = request.path.string();

        std::vector<std::string> extensions;
        extensions.reserve(request.extensions.size());
        for (const auto& extension : request.extensions) {
            extensions.push_back(lowerAscii(extension));
        }

        std::error_code ec;
        fs::directory_iterator iterator(request.path,
                                        fs::directory_options::skip_permission_denied, ec);
        if (ec) {
            directory.error = ec.message();
            result.directories.push_back(std::move(directory));
            continue;
        }

        try {
            for (const auto& entry : iterator) {
                if (!entry.is_regular_file()) {
                    continue;
                }
                auto name = entry.path().filename().string();
                auto base = name;
                bool ghost = false;
                if (lowerAscii(fs::path(base).extension().string()) == ".ghost") {
                    ghost = true;
                    base = fs::path(base).stem().string();
                }
                const auto extension = lowerAscii(fs::path(base).extension().string());
                if (std::find(extensions.begin(), extensions.end(), extension) ==
                    extensions.end()) {
                    continue;
                }
                directory.files.push_back(
                    {name, lowerAscii(fs::path(base).filename().string()), ghost});
            }
        } catch (const std::exception& err) {
            directory.error = err.what();
        }

        std::sort(directory.files.begin(), directory.files.end(),
                  [](const PluginFile& lhs, const PluginFile& rhs) {
                      return lowerAscii(lhs.name) < lowerAscii(rhs.name);
                  });
        result.directories.push_back(std::move(directory));
    }

    return result;
}

PluginOrderResult readPluginOrder(const fs::path& directory) {
    PluginOrderResult result;
    result.pluginsFile = existingFileName(directory, "plugins.txt");
    result.loadOrderFile = existingFileName(directory, "loadorder.txt");
    try {
        auto plugins = readRows(directory / result.pluginsFile);
        auto loadOrder = readRows(directory / result.loadOrderFile);
        result.pluginsHeader = std::move(plugins.header);
        result.plugins = std::move(plugins.rows);
        result.loadOrderHeader = std::move(loadOrder.header);
        result.loadOrder = std::move(loadOrder.rows);
    } catch (const std::exception& err) {
        result.error = err.what();
    }
    return result;
}

PluginOrderResult writePluginOrder(const fs::path& directory,
                                   const std::vector<std::string>& plugins,
                                   const std::vector<std::string>& loadOrder) {
    PluginOrderResult result;
    result.pluginsFile = existingFileName(directory, "plugins.txt");
    result.loadOrderFile = existingFileName(directory, "loadorder.txt");
    result.pluginsHeader = "# Automatically generated by Vortex";
    result.loadOrderHeader = "# Automatically generated by Vortex";
    result.plugins = plugins;
    result.loadOrder = loadOrder;

    try {
        writeAtomic(directory / result.loadOrderFile,
                    "# Automatically generated by Vortex\r\n" + joinRows(loadOrder));
        writeAtomic(directory / result.pluginsFile,
                    "# Automatically generated by Vortex\r\n" + joinRows(plugins) + "\r\n");
    } catch (const std::exception& err) {
        result.error = err.what();
    }
    return result;
}

} // namespace vortex::core
