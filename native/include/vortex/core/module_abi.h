#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VORTEX_MODULE_ABI_VERSION 1u
#define VORTEX_MODULE_ENTRY "vortex_module_entry"

typedef struct vortex_module_v1 {
    uint32_t abi_version;
    const char* name;
    const char* version;

    /**
     * Invoke a module method. `params_json` is a UTF-8 JSON object. The module
     * writes either a JSON result or an error message into an allocated string
     * and returns 0 on success or non-zero on failure. The host calls
     * `free_string` on whichever pointer was set.
     */
    int (*invoke)(const char* method, const char* params_json, char** result_json,
                  char** error_message);
    void (*free_string)(char* value);
} vortex_module_v1;

typedef const vortex_module_v1* (*vortex_module_entry_fn)(void);

#ifdef __cplusplus
}
#endif
