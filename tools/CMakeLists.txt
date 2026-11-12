set(PIME_TOOL_SUPPORT_SOURCES
    ${PROJECT_SOURCE_DIR}/src/util/base64.cpp
    ${PROJECT_SOURCE_DIR}/src/util/semver.cpp
    ${PROJECT_SOURCE_DIR}/src/util/sha256.cpp
    ${PROJECT_SOURCE_DIR}/src/util/status.cpp
    ${PROJECT_SOURCE_DIR}/src/util/string_util.cpp
    ${PROJECT_SOURCE_DIR}/src/util/utf8.cpp

    ${PROJECT_SOURCE_DIR}/src/codec/cbor.cpp
    ${PROJECT_SOURCE_DIR}/src/codec/manifest.cpp
    ${PROJECT_SOURCE_DIR}/src/codec/package.cpp
    ${PROJECT_SOURCE_DIR}/src/codec/signature.cpp
    ${PROJECT_SOURCE_DIR}/src/codec/yaml.cpp

    ${PROJECT_SOURCE_DIR}/src/capabilities/crypto/crypto.cpp

    ${PROJECT_SOURCE_DIR}/src/host/package.cpp

    ${PROJECT_SOURCE_DIR}/src/platform/common/platform.cpp
)

if(WIN32)
    list(APPEND PIME_TOOL_SUPPORT_SOURCES
         ${PROJECT_SOURCE_DIR}/src/platform/windows/platform_win.cpp)
else()
    list(APPEND PIME_TOOL_SUPPORT_SOURCES
         ${PROJECT_SOURCE_DIR}/src/platform/posix/platform_posix.cpp)
endif()

add_library(pime_tool_support STATIC ${PIME_TOOL_SUPPORT_SOURCES})

target_include_directories(pime_tool_support
    PUBLIC
        ${PROJECT_SOURCE_DIR}/src
        ${PROJECT_SOURCE_DIR}/src/codec
        ${PROJECT_SOURCE_DIR}/src/util
        ${PROJECT_SOURCE_DIR}/src/phl
        ${PROJECT_SOURCE_DIR}/src/platform
        ${PROJECT_SOURCE_DIR}/src/host
        ${PROJECT_SOURCE_DIR}/src/capabilities
        ${PROJECT_SOURCE_DIR}/include
        ${PIME_GENERATED_DIR}
)

target_compile_definitions(pime_tool_support PRIVATE
    PIME_CORE_VERSION="${PROJECT_VERSION}")

if(OpenSSL_FOUND)
    target_compile_definitions(pime_tool_support PRIVATE PIME_HAVE_OPENSSL)
    target_link_libraries(pime_tool_support PUBLIC OpenSSL::Crypto)
endif()

if(WIN32)
    target_link_libraries(pime_tool_support PUBLIC bcrypt)
endif()

if(MSVC)
    target_compile_options(pime_tool_support PRIVATE /W4 /permissive- /utf-8)
else()
    target_compile_options(pime_tool_support PRIVATE -Wall -Wextra -Wpedantic)
endif()

foreach(tool pimepack pimelint pimeabi pimebench)
    add_executable(${tool} ${tool}/main.cpp)
    target_link_libraries(${tool} PRIVATE pime_tool_support)
    set_target_properties(${tool} PROPERTIES OUTPUT_NAME ${tool})
endforeach()
