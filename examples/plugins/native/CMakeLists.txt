include(${CMAKE_CURRENT_SOURCE_DIR}/../../../sdk/cmake/PimePlugin.cmake)

pime_add_plugin(pime_uppercase
    RUNTIME native
    MANIFEST ${CMAKE_CURRENT_SOURCE_DIR}/manifest.yaml
    SOURCES
        plugin.cpp
        ../common/mini_cbor.hpp)

target_include_directories(pime_uppercase PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../common)
