include_guard()

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

set(ENABLE_APP OFF CACHE BOOL "" FORCE)
set(ENABLE_DOC OFF CACHE BOOL "" FORCE)
set(ENABLE_EXAMPLES OFF CACHE BOOL "" FORCE)
set(ENABLE_HPACK_TOOLS OFF CACHE BOOL "" FORCE)
set(ENABLE_LIB_ONLY ON CACHE BOOL "" FORCE)
set(ENABLE_TESTS OFF CACHE BOOL "" FORCE)
fiber_use_cached_content(nghttp2)
FetchContent_Declare(
    nghttp2
    URL https://github.com/nghttp2/nghttp2/archive/refs/tags/v1.68.0.tar.gz
)
FetchContent_MakeAvailable(nghttp2)

if (NOT TARGET nghttp2::nghttp2)
    if (TARGET nghttp2)
        get_target_property(_nghttp2_real nghttp2 ALIASED_TARGET)
        if (_nghttp2_real)
            add_library(nghttp2::nghttp2 ALIAS ${_nghttp2_real})
        else()
            add_library(nghttp2::nghttp2 ALIAS nghttp2)
        endif()
    elseif (TARGET nghttp2_static)
        add_library(nghttp2::nghttp2 ALIAS nghttp2_static)
    endif()
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
