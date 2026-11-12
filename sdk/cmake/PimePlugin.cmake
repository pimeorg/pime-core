# pime_add_plugin: builds a pime plugin and can assemble a package directory.
#
#   pime_add_plugin(<target>
#       RUNTIME <native|wasm|lua>
#       MANIFEST <path/to/manifest.yaml>
#       SOURCES <files...>
#       [ASSETS <files...>]          # copied to assets/
#       [MODELS <files...>]          # copied to models/
#       [PACKAGE_DIR <dir>])         # pacakge output, default <build>/package
#
# The core loads packages laid out as pime-plugin-<id>-<version>/.

include(CMakeParseArguments)

get_filename_component(PIME_SDK_CMAKE_DIR "${CMAKE_CURRENT_LIST_FILE}" DIRECTORY)
get_filename_component(PIME_ROOT "${PIME_SDK_CMAKE_DIR}/../.." ABSOLUTE)

function(pime_add_plugin target)
    set(options)
    set(one_value RUNTIME MANIFEST PACKAGE_DIR)
    set(multi_value SOURCES ASSETS MODELS)
    cmake_parse_arguments(PLUGIN "${options}" "${one_value}" "${multi_value}"
                          ${ARGN})

    if(NOT PLUGIN_MANIFEST)
        message(FATAL_ERROR "pime_add_plugin(${target}): MANIFEST is required")
    endif()
    if(NOT PLUGIN_RUNTIME)
        set(PLUGIN_RUNTIME native)
    endif()

    file(STRINGS "${PLUGIN_MANIFEST}" id_line REGEX "^id:")
    file(STRINGS "${PLUGIN_MANIFEST}" version_line REGEX "^version:")
    string(REGEX REPLACE "^id:[ \t]*" "" plugin_id "${id_line}")
    string(REGEX REPLACE "^version:[ \t]*" "" plugin_version "${version_line}")
    string(STRIP "${plugin_id}" plugin_id)
    string(STRIP "${plugin_version}" plugin_version)

    if(PLUGIN_RUNTIME STREQUAL "native")
        add_library(${target} MODULE ${PLUGIN_SOURCES})
        set_target_properties(${target} PROPERTIES
            PREFIX ""
            OUTPUT_NAME "plugin"
            CXX_VISIBILITY_PRESET hidden)
        target_include_directories(${target} PRIVATE
            ${PIME_ROOT}/include
            ${PIME_ROOT}/sdk/native/include)
    elseif(PLUGIN_RUNTIME STREQUAL "wasm")
        add_library(${target} MODULE ${PLUGIN_SOURCES})
        set_target_properties(${target} PROPERTIES PREFIX "" OUTPUT_NAME "plugin")
        target_include_directories(${target} PRIVATE
            ${PIME_ROOT}/sdk/wasm/include)
        # A WAMR target is expected to provide its own toolchain/wasi flags.
    elseif(PLUGIN_RUNTIME STREQUAL "lua")
        add_custom_target(${target} SOURCES ${PLUGIN_SOURCES})
    else()
        message(FATAL_ERROR "pime_add_plugin: unknown runtime '${PLUGIN_RUNTIME}'")
    endif()

    if(NOT PLUGIN_PACKAGE_DIR)
        set(PLUGIN_PACKAGE_DIR "${CMAKE_CURRENT_BINARY_DIR}/package")
    endif()
    set(package_root
        "${PLUGIN_PACKAGE_DIR}/pime-plugin-${plugin_id}-${plugin_version}")

    set(copy_commands
        COMMAND ${CMAKE_COMMAND} -E make_directory "${package_root}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${PLUGIN_MANIFEST}" "${package_root}/manifest.yaml")

    if(PLUGIN_RUNTIME STREQUAL "native")
        list(APPEND copy_commands
             COMMAND ${CMAKE_COMMAND} -E copy_if_different
                     "$<TARGET_FILE:${target}>" "${package_root}/$<TARGET_FILE_NAME:${target}>")
    elseif(PLUGIN_RUNTIME STREQUAL "wasm")
        list(APPEND copy_commands
             COMMAND ${CMAKE_COMMAND} -E copy_if_different
                     "$<TARGET_FILE:${target}>" "${package_root}/plugin.wasm")
    elseif(PLUGIN_RUNTIME STREQUAL "lua")
        foreach(source IN LISTS PLUGIN_SOURCES)
            get_filename_component(source_name "${source}" NAME)
            list(APPEND copy_commands
                 COMMAND ${CMAKE_COMMAND} -E copy_if_different
                         "${CMAKE_CURRENT_SOURCE_DIR}/${source}"
                         "${package_root}/${source_name}")
        endforeach()
    endif()

    if(PLUGIN_ASSETS)
        list(APPEND copy_commands
             COMMAND ${CMAKE_COMMAND} -E make_directory "${package_root}/assets")
        foreach(asset IN LISTS PLUGIN_ASSETS)
            list(APPEND copy_commands
                 COMMAND ${CMAKE_COMMAND} -E copy_if_different
                         "${CMAKE_CURRENT_SOURCE_DIR}/${asset}"
                         "${package_root}/assets/")
        endforeach()
    endif()

    if(PLUGIN_MODELS)
        list(APPEND copy_commands
             COMMAND ${CMAKE_COMMAND} -E make_directory "${package_root}/models")
        foreach(model IN LISTS PLUGIN_MODELS)
            list(APPEND copy_commands
                 COMMAND ${CMAKE_COMMAND} -E copy_if_different
                         "${CMAKE_CURRENT_SOURCE_DIR}/${model}"
                         "${package_root}/models/")
        endforeach()
    endif()

    add_custom_target(${target}_package ${copy_commands}
        DEPENDS ${target}
        COMMENT "Assembling package ${plugin_id} ${plugin_version}")
endfunction()
