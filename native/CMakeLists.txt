cmake_minimum_required(VERSION 3.28)

project(vortex_native LANGUAGES CXX)

set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    add_compile_options(-Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion)
endif()

option(VORTEX_ENABLE_SANITIZERS "Build with address and undefined-behaviour sanitizers" OFF)
if(VORTEX_ENABLE_SANITIZERS AND CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
endif()

add_library(vortex_deployment
    src/executor.cpp
    src/external_changes.cpp
    src/file_index.cpp
    src/journal.cpp
    src/manifest.cpp
    src/platform.cpp
    src/planner.cpp
)

target_include_directories(vortex_deployment
    PUBLIC
        include
        third_party
)

find_package(Threads REQUIRED)

add_library(vortex_module_support
    src/process.cpp
)
target_include_directories(vortex_module_support PUBLIC include third_party)
set_target_properties(vortex_module_support PROPERTIES POSITION_INDEPENDENT_CODE ON)

add_library(vortex_core
    src/archive.cpp
    src/collection.cpp
    src/deps.cpp
    src/download.cpp
    src/hash.cpp
    src/install.cpp
    src/module_host.cpp
    src/path_safety.cpp
    src/plugins.cpp
    src/status.cpp
)
target_link_libraries(vortex_core PUBLIC vortex_deployment vortex_module_support Threads::Threads ${CMAKE_DL_LIBS})
target_include_directories(vortex_core PUBLIC include)

add_library(vortex_game
    src/game_descriptor.cpp
    src/toml.cpp
)
target_include_directories(vortex_game PUBLIC include)

add_executable(vortex-deploy cli/main.cpp)
target_link_libraries(vortex-deploy PRIVATE vortex_deployment)

add_executable(vortex-core core/main.cpp)
target_link_libraries(vortex-core PRIVATE vortex_core vortex_game)

# `cmake --install` staging point used by the packaging step. Electron-builder
# picks this up as resources/native/vortex-core[.exe].
install(TARGETS vortex-core RUNTIME DESTINATION native)

add_executable(vortex-deployment-tests tests/main.cpp)
target_link_libraries(vortex-deployment-tests PRIVATE vortex_core vortex_game)

# Archive tests use the external 7-Zip binary, exactly like the core.
find_program(SEVEN_ZIP_EXECUTABLE NAMES 7z 7zz 7za)
if(SEVEN_ZIP_EXECUTABLE)
    target_compile_definitions(vortex-deployment-tests
        PRIVATE VORTEX_7Z_PATH="${SEVEN_ZIP_EXECUTABLE}"
    )
endif()
target_compile_definitions(vortex-deployment-tests PRIVATE
    VORTEX_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/data"
)

add_library(vortex_node_helper_module MODULE modules/node_helper.cpp)
target_link_libraries(vortex_node_helper_module PRIVATE vortex_module_support)
target_include_directories(vortex_node_helper_module PRIVATE include third_party)

add_library(vortex_test_echo_module MODULE tests/modules/echo_module.cpp)
target_include_directories(vortex_test_echo_module PRIVATE include)
target_compile_definitions(vortex-deployment-tests PRIVATE
    VORTEX_TEST_ECHO_MODULE="$<TARGET_FILE:vortex_test_echo_module>"
)

enable_testing()
add_test(NAME vortex-deployment-tests COMMAND vortex-deployment-tests)

find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    add_test(
        NAME vortex-core-protocol
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/core_protocol.py
                $<TARGET_FILE:vortex-core>
    )
endif()

find_program(NODE_EXECUTABLE node)
if(NODE_EXECUTABLE)
    add_test(
        NAME vortex-core-node-client
        COMMAND ${NODE_EXECUTABLE} --test ${CMAKE_CURRENT_SOURCE_DIR}/clients/vortex-core-client.test.mjs
    )
    set_tests_properties(vortex-core-node-client PROPERTIES
        ENVIRONMENT "VORTEX_CORE_PATH=$<TARGET_FILE:vortex-core>"
    )
endif()
