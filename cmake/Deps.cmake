include_guard()

include(ExternalProject)
include(FetchContent)

if (NOT DEFINED FETCHCONTENT_BASE_DIR)
    set(FETCHCONTENT_BASE_DIR "${CMAKE_CURRENT_LIST_DIR}/../temp/_deps" CACHE PATH "FetchContent base directory")
endif()

function(fiber_clear_invalid_source_dir name)
    string(TOUPPER "${name}" name_upper)
    set(var "FETCHCONTENT_SOURCE_DIR_${name_upper}")
    if (DEFINED ${var})
        if ("${${var}}" STREQUAL "" OR NOT EXISTS "${${var}}/CMakeLists.txt")
            unset(${var} CACHE)
        endif()
    endif()
endfunction()

function(fiber_use_cached_content name)
    string(TOUPPER "${name}" name_upper)
    set(var "FETCHCONTENT_SOURCE_DIR_${name_upper}")
    fiber_clear_invalid_source_dir("${name}")
    if (NOT DEFINED ${var})
        set(src "${FETCHCONTENT_BASE_DIR}/${name}-src")
        if (EXISTS "${src}/CMakeLists.txt")
            set(${var} "${src}" CACHE PATH "Use cached ${name} source directory")
        endif()
    endif()
endfunction()

function(fiber_purge_cache_regex pattern)
    get_cmake_property(cache_vars CACHE_VARIABLES)
    foreach(cache_var IN LISTS cache_vars)
        if (cache_var MATCHES "${pattern}")
            unset(${cache_var} CACHE)
        endif()
    endforeach()
endfunction()

function(fiber_prepare_jemalloc_target)
    if (TARGET fiber_jemalloc)
        return()
    endif()

    set(FIBER_JEMALLOC_VERSION "5.3.0")
    set(FIBER_JEMALLOC_SOURCE_DIR "${FETCHCONTENT_BASE_DIR}/jemalloc-src")
    set(FIBER_JEMALLOC_BINARY_DIR "${CMAKE_BINARY_DIR}/_deps/jemalloc-build")
    set(FIBER_JEMALLOC_INSTALL_DIR "${CMAKE_BINARY_DIR}/_deps/jemalloc-install")

    ExternalProject_Add(
        fiber_jemalloc_ep
        URL "https://github.com/jemalloc/jemalloc/releases/download/${FIBER_JEMALLOC_VERSION}/jemalloc-${FIBER_JEMALLOC_VERSION}.tar.bz2"
        SOURCE_DIR "${FIBER_JEMALLOC_SOURCE_DIR}"
        BINARY_DIR "${FIBER_JEMALLOC_BINARY_DIR}"
        INSTALL_DIR "${FIBER_JEMALLOC_INSTALL_DIR}"
        CONFIGURE_COMMAND
            ${CMAKE_COMMAND} -E env CC=${CMAKE_C_COMPILER}
            <SOURCE_DIR>/configure
            --prefix=<INSTALL_DIR>
            --disable-shared
            --enable-static
        BUILD_COMMAND ${CMAKE_MAKE_PROGRAM}
        INSTALL_COMMAND ${CMAKE_MAKE_PROGRAM} install
        BUILD_BYPRODUCTS "${FIBER_JEMALLOC_INSTALL_DIR}/lib/libjemalloc.a"
        UPDATE_DISCONNECTED ON
    )

    add_library(fiber_jemalloc UNKNOWN IMPORTED GLOBAL)
    set_target_properties(fiber_jemalloc PROPERTIES
        IMPORTED_LOCATION "${FIBER_JEMALLOC_INSTALL_DIR}/lib/libjemalloc.a")
endfunction()

set(FETCHCONTENT_UPDATES_DISCONNECTED ON)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(BUILD_STATIC_LIBS ON CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set_property(GLOBAL PROPERTY ALLOW_DUPLICATE_CUSTOM_TARGETS ON)

set(BORINGSSL_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(BORINGSSL_INSTALL OFF CACHE BOOL "" FORCE)
fiber_use_cached_content(boringssl)
FetchContent_Declare(
    boringssl
    URL https://github.com/google/boringssl/archive/refs/tags/0.20251124.0.tar.gz
)
FetchContent_MakeAvailable(boringssl)

if (TARGET ssl AND NOT TARGET boringssl::ssl)
    add_library(boringssl::ssl ALIAS ssl)
endif()
if (TARGET crypto AND NOT TARGET boringssl::crypto)
    add_library(boringssl::crypto ALIAS crypto)
endif()

set(BORINGSSL_INCLUDE_DIR "${boringssl_SOURCE_DIR}/include" CACHE PATH "" FORCE)
set(BORINGSSL_LIBRARY_DIR "${boringssl_BINARY_DIR}" CACHE PATH "" FORCE)
set(BORINGSSL_ROOT_DIR "${boringssl_BINARY_DIR}" CACHE PATH "" FORCE)
set(LSQUIC_BIN OFF CACHE BOOL "" FORCE)
set(LSQUIC_TESTS OFF CACHE BOOL "" FORCE)
set(LSQUIC_SHARED_LIB OFF CACHE BOOL "" FORCE)
set(LSQUIC_DEVEL OFF CACHE BOOL "" FORCE)
set(LSQUIC_WEBTRANSPORT OFF CACHE BOOL "" FORCE)
set(LSQUIC_LIBSSL BORINGSSL CACHE STRING "" FORCE)
set(LIBSSL_DIR "${boringssl_SOURCE_DIR}" CACHE PATH "" FORCE)
set(LIBSSL_LIB "${boringssl_BINARY_DIR}" CACHE PATH "" FORCE)
set(LIBSSL_LIB_ssl ssl CACHE STRING "" FORCE)
set(LIBSSL_LIB_crypto crypto CACHE STRING "" FORCE)
find_path(FIBER_ZLIB_INCLUDE_DIR NAMES zlib.h)
find_library(FIBER_ZLIB_LIBRARY NAMES z)
set(FIBER_HAVE_LSQUIC OFF)
if (FIBER_ZLIB_INCLUDE_DIR AND FIBER_ZLIB_LIBRARY)
    fiber_use_cached_content(lsquic)
    FetchContent_Declare(
        lsquic
        GIT_REPOSITORY https://github.com/litespeedtech/lsquic.git
        GIT_TAG v4.5.0
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(lsquic)

    if (NOT TARGET lsquic::lsquic)
        if (TARGET lsquic)
            get_target_property(_lsquic_real lsquic ALIASED_TARGET)
            if (_lsquic_real)
                add_library(lsquic::lsquic ALIAS ${_lsquic_real})
            else()
                add_library(lsquic::lsquic ALIAS lsquic)
            endif()
        elseif (TARGET lsquic_static)
            add_library(lsquic::lsquic ALIAS lsquic_static)
        endif()
    endif()

    if (TARGET lsquic::lsquic)
        set(FIBER_HAVE_LSQUIC ON)
    endif()
else()
    message(STATUS "Skipping lsquic dependency because zlib development files were not found")
endif()
