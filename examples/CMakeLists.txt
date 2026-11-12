cmake_minimum_required(VERSION 3.20)
project(pime_examples LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_subdirectory(plugins/native)
add_subdirectory(plugins/lua)
add_subdirectory(plugins/wasm)

option(PIME_BUILD_EXAMPLE_HOST "Build the minimal host example" ON)
if(PIME_BUILD_EXAMPLE_HOST)
    add_subdirectory(hosts/minimal)
endif()
