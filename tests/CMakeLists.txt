add_library(pime_test_plugin SHARED
    plugin/test_plugin.cpp
    ${CMAKE_SOURCE_DIR}/src/codec/cbor.cpp
)
target_include_directories(pime_test_plugin PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/sdk/native/include
    ${CMAKE_SOURCE_DIR}/src
)
set_target_properties(pime_test_plugin PROPERTIES
    PREFIX ""
    OUTPUT_NAME "plugin"
)
if(APPLE)
    set_target_properties(pime_test_plugin PROPERTIES SUFFIX ".dylib")
endif()

add_library(pime_core_internal STATIC
    ${CMAKE_SOURCE_DIR}/src/util/base64.cpp
    ${CMAKE_SOURCE_DIR}/src/util/semver.cpp
    ${CMAKE_SOURCE_DIR}/src/util/sha256.cpp
    ${CMAKE_SOURCE_DIR}/src/util/status.cpp
    ${CMAKE_SOURCE_DIR}/src/util/string_util.cpp
    ${CMAKE_SOURCE_DIR}/src/util/utf8.cpp
    ${CMAKE_SOURCE_DIR}/src/codec/cbor.cpp
    ${CMAKE_SOURCE_DIR}/src/codec/manifest.cpp
    ${CMAKE_SOURCE_DIR}/src/codec/package.cpp
    ${CMAKE_SOURCE_DIR}/src/codec/signature.cpp
    ${CMAKE_SOURCE_DIR}/src/codec/yaml.cpp
    ${CMAKE_SOURCE_DIR}/src/phl/buffer_pool.cpp
    ${CMAKE_SOURCE_DIR}/src/phl/channel.cpp
    ${CMAKE_SOURCE_DIR}/src/phl/context_cache.cpp
    ${CMAKE_SOURCE_DIR}/src/phl/shared_buffer.cpp
    ${CMAKE_SOURCE_DIR}/src/phl/sync.cpp
    ${CMAKE_SOURCE_DIR}/src/phl/thread.cpp
    ${CMAKE_SOURCE_DIR}/src/capabilities/workspace/workspace.cpp
    ${CMAKE_SOURCE_DIR}/src/platform/common/platform.cpp
)
if(WIN32)
    target_sources(pime_core_internal PRIVATE
        ${CMAKE_SOURCE_DIR}/src/platform/windows/platform_win.cpp)
else()
    target_sources(pime_core_internal PRIVATE
        ${CMAKE_SOURCE_DIR}/src/platform/posix/platform_posix.cpp)
endif()
target_include_directories(pime_core_internal PUBLIC
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/codec
    ${CMAKE_SOURCE_DIR}/src/util
    ${CMAKE_SOURCE_DIR}/src/phl
    ${CMAKE_SOURCE_DIR}/src/platform
    ${CMAKE_SOURCE_DIR}/src/capabilities
    ${CMAKE_SOURCE_DIR}/src/capabilities/workspace
)
target_compile_definitions(pime_core_internal PUBLIC PIME_TEST_INTERNAL)
if(WIN32)
    target_link_libraries(pime_core_internal PUBLIC bcrypt)
endif()

add_executable(pime_tests
    test_main.cpp
    test_util.cpp
    test_codec.cpp
    test_phl.cpp
    test_core.cpp
)
target_link_libraries(pime_tests PRIVATE pime_core pime_core_internal)
target_include_directories(pime_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/codec
    ${CMAKE_SOURCE_DIR}/src/util
    ${CMAKE_SOURCE_DIR}/src/phl
    ${CMAKE_SOURCE_DIR}/src/capabilities
)
target_compile_definitions(pime_tests PRIVATE
    TEST_PLUGIN_PATH="$<TARGET_FILE:pime_test_plugin>"
)
add_dependencies(pime_tests pime_test_plugin)

add_test(NAME pime_tests COMMAND pime_tests)
set_tests_properties(pime_tests PROPERTIES TIMEOUT 60)
