# Building a WASM plugin requires a WASI toolchain (for example
# wasi-sdk) supplied through CMAKE_TOOLCHAIN_FILE, or the clang command in
# plugin.cpp. Without one this example is skipped.
if(NOT CMAKE_SYSTEM_PROCESSOR MATCHES "wasm32")
    message(STATUS "pime example wasm plugin: skipped (not a wasm32 toolchain)")
    return()
endif()

include(${CMAKE_CURRENT_SOURCE_DIR}/../../../sdk/cmake/PimePlugin.cmake)

pime_add_plugin(pime_uppercase_wasm
    RUNTIME wasm
    MANIFEST ${CMAKE_CURRENT_SOURCE_DIR}/manifest.yaml
    SOURCES plugin.cpp)
