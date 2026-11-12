include(${CMAKE_CURRENT_SOURCE_DIR}/../../../sdk/cmake/PimePlugin.cmake)

pime_add_plugin(pime_uppercase_lua
    RUNTIME lua
    MANIFEST ${CMAKE_CURRENT_SOURCE_DIR}/manifest.yaml
    SOURCES plugin.lua)
