#include "vortex/core/module_abi.h"
#include "vortex/core/process.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstring>
#include <stdexcept>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>

namespace {

using nlohmann::json;
namespace fs = std::filesystem;

char* copyString(const std::string& value) {
    auto* result = static_cast<char*>(std::malloc(value.size() + 1));
    if (result != nullptr) {
        std::memcpy(result, value.c_str(), value.size() + 1);
    }
    return result;
}

std::string uniqueRequestPath() {
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    return (fs::temp_directory_path() /
            fs::path("vortex-helper-" + std::to_string(now) + ".json"))
        .string();
}

int invoke(const char* method, const char* rawParams, char** result, char** error) {
    try {
        const auto name = std::string(method == nullptr ? "" : method);
        if (name != "call" && name != "fomod.resolve" && name != "metadata.lookup" &&
            name != "fomod.testSupported") {
            throw std::runtime_error("unknown node-helper method: " + name);
        }

        const auto params = json::parse(rawParams == nullptr ? "{}" : rawParams);
        const auto node = params.value("node", std::string("node"));
        const auto script = params.value("script", std::string{});
        std::map<std::string, std::string> environment;
        if (params.contains("env") && params["env"].is_object()) {
            for (const auto& [key, value] : params["env"].items()) {
                if (value.is_string()) {
                    environment[key] = value.get<std::string>();
                }
            }
        }
        if (script.empty()) {
            throw std::runtime_error("node-helper requires params.script");
        }

        json request;
        if (name == "call") {
            request = params.value("request", json::object());
        } else {
            request = {{"method", name}, {"params", params}};
        }

        const auto requestPath = uniqueRequestPath();
        {
            std::ofstream output(requestPath, std::ios::binary | std::ios::trunc);
            if (!output) {
                throw std::runtime_error("failed to create helper request: " + requestPath);
            }
            output << request.dump();
            output.flush();
            if (!output) {
                throw std::runtime_error("failed to write helper request: " + requestPath);
            }
        }

        const auto process = vortex::core::runProcess(
            fs::path(node), {script, "--request", requestPath}, nullptr, environment);
        std::error_code cleanup;
        fs::remove(requestPath, cleanup);

        if (process.exitCode != 0) {
            throw std::runtime_error(process.output.empty() ? "node helper failed"
                                                            : process.output);
        }
        const auto jsonStart = process.output.find_first_of('{');
        if (jsonStart == std::string::npos) {
            throw std::runtime_error(process.output.empty() ? "node helper produced no output"
                                                            : process.output);
        }
        const auto response = json::parse(process.output.substr(jsonStart));
        if (response.contains("error")) {
            throw std::runtime_error(response["error"].get<std::string>());
        }
        const auto value = response.value("result", json::object());
        *result = copyString(value.dump());
        return 0;
    } catch (const std::exception& err) {
        *error = copyString(err.what());
        return 1;
    }
}

void freeString(char* value) {
    std::free(value);
}

const vortex_module_v1 module = {
    VORTEX_MODULE_ABI_VERSION,
    "node-helper",
    "1.0.0",
    &invoke,
    &freeString,
};

} // namespace

extern "C" const vortex_module_v1* vortex_module_entry(void) {
    return &module;
}
