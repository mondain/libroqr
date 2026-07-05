# FindPicoquic.cmake - provides picoquic::picoquic-core.
# Resolution order:
#   1. Target already defined by the enclosing project.
#   2. Installed config package: find_package(picoquic CONFIG).
#   3. Source tree: -DROQR_PICOQUIC_SOURCE_DIR=/path (env var of the same
#      name is honored), with picotls resolved via ROQR_PICOTLS_PREFIX.

if(TARGET picoquic::picoquic-core)
    set(Picoquic_FOUND TRUE)
    return()
endif()

find_package(picoquic CONFIG QUIET)
if(picoquic_FOUND)
    if(NOT TARGET picoquic::picoquic-core AND TARGET picoquic-core)
        add_library(picoquic::picoquic-core ALIAS picoquic-core)
    endif()
    set(Picoquic_FOUND TRUE)
    return()
endif()

if(NOT ROQR_PICOQUIC_SOURCE_DIR AND DEFINED ENV{ROQR_PICOQUIC_SOURCE_DIR})
    set(ROQR_PICOQUIC_SOURCE_DIR "$ENV{ROQR_PICOQUIC_SOURCE_DIR}")
endif()
if(NOT ROQR_PICOTLS_PREFIX AND DEFINED ENV{ROQR_PICOTLS_PREFIX})
    set(ROQR_PICOTLS_PREFIX "$ENV{ROQR_PICOTLS_PREFIX}")
endif()

if(ROQR_PICOQUIC_SOURCE_DIR)
    if(NOT ROQR_PICOTLS_PREFIX)
        set(ROQR_PICOTLS_PREFIX "${ROQR_PICOQUIC_SOURCE_DIR}/../picotls/build")
    endif()
    # picoquic's CMake finds picotls via PTLS_* hints.
    set(PTLS_PREFIX "${ROQR_PICOTLS_PREFIX}" CACHE PATH "" FORCE)
    list(APPEND CMAKE_PREFIX_PATH "${ROQR_PICOTLS_PREFIX}")

    set(BUILD_DEMO OFF CACHE BOOL "" FORCE)
    set(BUILD_PQBENCH OFF CACHE BOOL "" FORCE)
    set(BUILD_LOGLIB ON CACHE BOOL "" FORCE)
    set(BUILD_HTTP OFF CACHE BOOL "" FORCE)
    set(picoquic_BUILD_TESTS OFF CACHE BOOL "" FORCE)

    add_subdirectory("${ROQR_PICOQUIC_SOURCE_DIR}" picoquic-build EXCLUDE_FROM_ALL)

    if(NOT TARGET picoquic::picoquic-core AND TARGET picoquic-core)
        add_library(picoquic::picoquic-core ALIAS picoquic-core)
    endif()
    if(TARGET picoquic-log)
        set_property(TARGET picoquic-core APPEND PROPERTY
            INTERFACE_LINK_LIBRARIES picoquic-log)
    endif()
    set(Picoquic_FOUND TRUE)
    return()
endif()

set(Picoquic_FOUND FALSE)
if(Picoquic_FIND_REQUIRED)
    message(FATAL_ERROR
        "picoquic not found. Install it, or run: eval \"$(scripts/setup_picoquic_deps.sh)\" and re-configure.")
endif()
