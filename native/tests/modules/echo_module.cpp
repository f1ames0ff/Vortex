#include "vortex/core/module_abi.h"

#include <cstdlib>
#include <cstring>
#include <string>

namespace {

char* copyString(const std::string& value) {
    auto* result = static_cast<char*>(std::malloc(value.size() + 1));
    if (result != nullptr) {
        std::memcpy(result, value.c_str(), value.size() + 1);
    }
    return result;
}

int invoke(const char* method, const char* params, char** result, char** error) {
    const std::string name = method == nullptr ? "" : method;
    if (name == "echo") {
        *result = copyString(params == nullptr ? "{}" : params);
        return 0;
    }
    if (name == "meta.describe") {
        *result = copyString(R"({"module":"echo","methods":["echo","meta.describe"]})");
        return 0;
    }
    *error = copyString("unknown echo-module method: " + name);
    return 1;
}

void freeString(char* value) {
    std::free(value);
}

const vortex_module_v1 module = {
    VORTEX_MODULE_ABI_VERSION,
    "echo",
    "1.0.0",
    &invoke,
    &freeString,
};

} // namespace

extern "C" const vortex_module_v1* vortex_module_entry(void) {
    return &module;
}
