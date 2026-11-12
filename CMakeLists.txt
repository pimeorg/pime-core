cmake_minimum_required(VERSION 3.25)

project(pime_core VERSION 1.0.0 LANGUAGES C CXX)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

option(PIME_BUILD_SHARED "Build pime_core as a shared library" ON)
option(PIME_BUILD_WASM "Enable the WAMR WASM plugin runtime" OFF)
option(PIME_BUILD_LUA "Enable the Lua plugin runtime" OFF)
option(PIME_BUILD_TESTS "Build pime_core tests" OFF)
option(PIME_BUILD_TOOLS "Build pime tooling" OFF)
option(PIME_SANITIZE_ADDRESS "Enable AddressSanitizer" OFF)
option(PIME_SANITIZE_UNDEFINED "Enable UndefinedBehaviorSanitizer" OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(PIME_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${PIME_GENERATED_DIR}/pime")
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/include/pime/version.h.in"
               "${PIME_GENERATED_DIR}/pime/version.h" @ONLY)

set(PIME_SOURCES
    src/util/base64.cpp
    src/util/semver.cpp
    src/util/sha256.cpp
    src/util/status.cpp
    src/util/string_util.cpp
    src/util/utf8.cpp

    src/codec/cbor.cpp
    src/codec/manifest.cpp
    src/codec/package.cpp
    src/codec/signature.cpp
    src/codec/yaml.cpp

    src/phl/buffer_pool.cpp
    src/phl/channel.cpp
    src/phl/context_cache.cpp
    src/phl/shared_buffer.cpp
    src/phl/sync.cpp
    src/phl/thread.cpp

    src/platform/common/platform.cpp
    src/capabilities/asset/asset.cpp
    src/capabilities/audio/opus.cpp
    src/capabilities/audio/pcm.cpp
    src/capabilities/audio/stream.cpp
    src/capabilities/caps/caps.cpp
    src/capabilities/config/config.cpp
    src/capabilities/crypto/crypto.cpp
    src/capabilities/log/log.cpp
    src/capabilities/metric/metric.cpp
    src/capabilities/ml/ml.cpp
    src/capabilities/net/net.cpp
    src/capabilities/permission/permission.cpp
    src/capabilities/store/store.cpp
    src/capabilities/time/time.cpp
    src/capabilities/timer/timer.cpp
    src/capabilities/workspace/workspace.cpp

    src/host/native.cpp
    src/host/package.cpp

    src/core/core.cpp
    src/core/generation.cpp
    src/core/host_bridge.cpp

    src/hal/hal_core.cpp
    src/hal/hal_plugin.cpp
    src/hal/hal_session.cpp
)

if(WIN32)
    list(APPEND PIME_SOURCES src/platform/windows/platform_win.cpp)
else()
    list(APPEND PIME_SOURCES src/platform/posix/platform_posix.cpp)
endif()

if(PIME_BUILD_SHARED)
    add_library(pime_core SHARED ${PIME_SOURCES})
    target_compile_definitions(pime_core PUBLIC PIME_SHARED)
    target_compile_definitions(pime_core PRIVATE PIME_CORE_BUILD)
else()
    add_library(pime_core STATIC ${PIME_SOURCES})
endif()

add_library(pime::core ALIAS pime_core)

target_include_directories(pime_core
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/src/codec
        ${CMAKE_CURRENT_SOURCE_DIR}/src/util
        ${CMAKE_CURRENT_SOURCE_DIR}/src/phl
        ${CMAKE_CURRENT_SOURCE_DIR}/src/platform
        ${CMAKE_CURRENT_SOURCE_DIR}/src/host
        ${CMAKE_CURRENT_SOURCE_DIR}/src/core
        ${CMAKE_CURRENT_SOURCE_DIR}/src/hal
        ${CMAKE_CURRENT_SOURCE_DIR}/src/capabilities
        ${CMAKE_CURRENT_SOURCE_DIR}/src/capabilities/asset
        ${CMAKE_CURRENT_SOURCE_DIR}/src/capabilities/audio
        ${CMAKE_CURRENT_SOURCE_DIR}/src/capabilities/caps
        ${CMAKE_CURRENT_SOURCE_DIR}/src/capabilities/config
        ${CMAKE_CURRENT_SOURCE_DIR}/src/capabilities/crypto
        ${CMAKE_CURRENT_SOURCE_DIR}/src/capabilities/log
        ${CMAKE_CURRENT_SOURCE_DIR}/src/capabilities/metric
        ${CMAKE_CURRENT_SOURCE_DIR}/src/capabilities/ml
        ${CMAKE_CURRENT_SOURCE_DIR}/src/capabilities/net
        ${CMAKE_CURRENT_SOURCE_DIR}/src/capabilities/permission
        ${CMAKE_CURRENT_SOURCE_DIR}/src/capabilities/store
        ${CMAKE_CURRENT_SOURCE_DIR}/src/capabilities/time
        ${CMAKE_CURRENT_SOURCE_DIR}/src/capabilities/timer
        ${CMAKE_CURRENT_SOURCE_DIR}/src/capabilities/workspace
        ${CMAKE_CURRENT_SOURCE_DIR}/sdk/native/include
        ${PIME_GENERATED_DIR}
)

target_compile_definitions(pime_core PRIVATE PIME_CORE_VERSION="${PROJECT_VERSION}")

if(MSVC)
    target_compile_options(pime_core PRIVATE /W4 /permissive- /utf-8)
else()
    target_compile_options(pime_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(PIME_SANITIZE_ADDRESS)
    if(MSVC)
        message(FATAL_ERROR "PIME_SANITIZE_ADDRESS is not supported with MSVC")
    endif()
    target_compile_options(pime_core PRIVATE -fsanitize=address -fno-omit-frame-pointer)
    target_link_options(pime_core PRIVATE -fsanitize=address)
endif()
if(PIME_SANITIZE_UNDEFINED)
    if(MSVC)
        message(FATAL_ERROR "PIME_SANITIZE_UNDEFINED is not supported with MSVC")
    endif()
    target_compile_options(pime_core PRIVATE -fsanitize=undefined -fno-omit-frame-pointer)
    target_link_options(pime_core PRIVATE -fsanitize=undefined)
endif()

# Optional third-party capabilities. Absent libraries compile to explicit
# PIME_UNSUPPORTED; nothing falls back silently.
find_package(OpenSSL QUIET)
if(OpenSSL_FOUND)
    target_compile_definitions(pime_core PRIVATE PIME_HAVE_OPENSSL)
    target_link_libraries(pime_core PRIVATE OpenSSL::Crypto)
endif()

find_package(CURL QUIET)
if(CURL_FOUND)
    target_compile_definitions(pime_core PRIVATE PIME_HAVE_CURL)
    target_link_libraries(pime_core PRIVATE CURL::libcurl)
endif()

find_path(PIME_LWS_INCLUDE_DIR libwebsockets.h)
find_library(PIME_LWS_LIBRARY NAMES websockets libwebsockets)
if(PIME_LWS_INCLUDE_DIR AND PIME_LWS_LIBRARY)
    target_compile_definitions(pime_core PRIVATE PIME_HAVE_LWS)
    target_include_directories(pime_core PRIVATE ${PIME_LWS_INCLUDE_DIR})
    target_link_libraries(pime_core PRIVATE ${PIME_LWS_LIBRARY})
endif()

find_path(PIME_OPUS_INCLUDE_DIR opus.h PATH_SUFFIXES opus)
find_library(PIME_OPUS_LIBRARY NAMES opus)
if(PIME_OPUS_INCLUDE_DIR AND PIME_OPUS_LIBRARY)
    target_compile_definitions(pime_core PRIVATE PIME_HAVE_OPUS)
    target_include_directories(pime_core PRIVATE ${PIME_OPUS_INCLUDE_DIR})
    target_link_libraries(pime_core PRIVATE ${PIME_OPUS_LIBRARY})
endif()

if(PIME_BUILD_WASM)
    find_path(PIME_WAMR_INCLUDE_DIR wasm_export.h)
    find_library(PIME_WAMR_LIBRARY NAMES vmlib iwasm)
    if(NOT PIME_WAMR_INCLUDE_DIR OR NOT PIME_WAMR_LIBRARY)
        message(FATAL_ERROR "PIME_BUILD_WASM requires WAMR headers and library")
    endif()
    target_compile_definitions(pime_core PRIVATE PIME_BUILD_WASM)
    target_sources(pime_core PRIVATE src/host/wasm/wasm_instance.cpp)
    target_include_directories(pime_core PRIVATE
        ${PIME_WAMR_INCLUDE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/sdk/wasm/include)
    target_link_libraries(pime_core PRIVATE ${PIME_WAMR_LIBRARY})
endif()

if(PIME_BUILD_LUA)
    find_package(Lua 5.4 QUIET)
    if(NOT Lua_FOUND)
        message(FATAL_ERROR "PIME_BUILD_LUA requires Lua 5.4")
    endif()
    target_compile_definitions(pime_core PRIVATE PIME_BUILD_LUA)
    target_sources(pime_core PRIVATE src/host/lua/lua_instance.cpp)
    target_link_libraries(pime_core PRIVATE ${LUA_LIBRARIES})
    target_include_directories(pime_core PRIVATE
        ${LUA_INCLUDE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/sdk/lua)
endif()

if(WIN32)
    target_link_libraries(pime_core PRIVATE bcrypt)
endif()

set_target_properties(pime_core PROPERTIES
    OUTPUT_NAME pime_core
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
)

install(TARGETS pime_core
    EXPORT pime_core_targets
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)
install(DIRECTORY include/pime DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(DIRECTORY sdk/native/include/pime DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/pime/sdk)
install(EXPORT pime_core_targets
    FILE pime_core_targets.cmake
    NAMESPACE pime::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/pime_core
)

if(PIME_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(PIME_BUILD_TOOLS)
    add_subdirectory(tools)
endif()
