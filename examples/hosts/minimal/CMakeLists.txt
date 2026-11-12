# The host links the already-built core library.
#
#   cmake -S examples -B examples/build \
#         -DPIME_CORE_LIBRARY=/path/to/libpime_core.so
#
# PIME_CORE_INCLUDE_DIR defaults to the include/ directory of this checkout.

set(PIME_CORE_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../../include"
    CACHE PATH "pime-core public headers")

find_library(PIME_CORE_LIBRARY
    NAMES pime_core
    PATHS ${PIME_CORE_LIBRARY_DIR}
    NO_DEFAULT_PATH)
if(NOT PIME_CORE_LIBRARY)
    message(STATUS "pime minimal host: skipped (set PIME_CORE_LIBRARY)")
    return()
endif()

add_executable(pime_minimal_host main.cpp)
target_include_directories(pime_minimal_host PRIVATE ${PIME_CORE_INCLUDE_DIR})
target_link_libraries(pime_minimal_host PRIVATE ${PIME_CORE_LIBRARY})
