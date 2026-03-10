cmake_minimum_required(VERSION 4.1)

set(FIBER_CLANG_BIN "/usr/bin")
set(FIBER_LLVM_ROOT "/usr/lib/llvm-21")
if (NOT DEFINED CMAKE_C_COMPILER AND EXISTS "${FIBER_CLANG_BIN}/clang-21")
    set(CMAKE_C_COMPILER "${FIBER_CLANG_BIN}/clang-21" CACHE FILEPATH "Default clang C compiler")
endif()
if (NOT DEFINED CMAKE_CXX_COMPILER AND EXISTS "${FIBER_CLANG_BIN}/clang++-21")
    set(CMAKE_CXX_COMPILER "${FIBER_CLANG_BIN}/clang++-21" CACHE FILEPATH "Default clang C++ compiler")
endif()
set(FIBER_CLANG_LIBDIR "${FIBER_LLVM_ROOT}/lib")
set(FIBER_CLANG_INCLUDEDIR "${FIBER_LLVM_ROOT}/include/c++/v1")
set(FIBER_USE_LIBCXX OFF)
if (CMAKE_CXX_COMPILER STREQUAL "${FIBER_CLANG_BIN}/clang++-21"
    AND EXISTS "${FIBER_CLANG_LIBDIR}"
    AND EXISTS "${FIBER_CLANG_INCLUDEDIR}")
    set(FIBER_USE_LIBCXX ON)
    set(CMAKE_CXX_FLAGS_INIT
        "${CMAKE_CXX_FLAGS_INIT} -stdlib=libc++ -isystem ${FIBER_CLANG_INCLUDEDIR}")
    set(FIBER_STDLIB_LINK_FLAGS -stdlib=libc++ -L${FIBER_CLANG_LIBDIR} -lc++abi -Wl,-rpath,${FIBER_CLANG_LIBDIR})
endif()

project(fiber)

set(FIBER_DEPS_DIR "${CMAKE_CURRENT_LIST_DIR}/temp/_deps" CACHE PATH "Shared deps download directory")
set(FETCHCONTENT_BASE_DIR "${FIBER_DEPS_DIR}" CACHE PATH "FetchContent base directory")
option(FIBER_FETCH_DEPS "Allow CMake to download third-party dependencies" ON)
option(FIBER_USE_JEMALLOC "Link final executables against jemalloc" OFF)

include(cmake/Deps.cmake)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
file(GLOB_RECURSE CPP_SRC_LIST ${CMAKE_CURRENT_LIST_DIR}/src/*.cpp)

add_library(fiber_lib STATIC ${CPP_SRC_LIST})
add_library(fiber_allocator INTERFACE)
target_include_directories(fiber_lib PUBLIC ${CMAKE_CURRENT_LIST_DIR}/src)
target_link_libraries(fiber_lib PUBLIC boringssl::ssl boringssl::crypto nghttp2::nghttp2)
if (FIBER_USE_LIBCXX)
    target_link_options(fiber_lib INTERFACE ${FIBER_STDLIB_LINK_FLAGS})
endif()

if (FIBER_USE_JEMALLOC)
    set(FIBER_JEMALLOC_TARGET "")
    find_package(jemalloc CONFIG QUIET)
    if (TARGET jemalloc::jemalloc)
        set(FIBER_JEMALLOC_TARGET jemalloc::jemalloc)
    elseif (TARGET jemalloc)
        set(FIBER_JEMALLOC_TARGET jemalloc)
    endif()

    if (NOT FIBER_JEMALLOC_TARGET)
        find_package(PkgConfig QUIET)
        if (PkgConfig_FOUND)
            pkg_check_modules(JEMALLOC QUIET IMPORTED_TARGET jemalloc)
            if (TARGET PkgConfig::JEMALLOC)
                set(FIBER_JEMALLOC_TARGET PkgConfig::JEMALLOC)
            endif()
        endif()
    endif()

    if (NOT FIBER_JEMALLOC_TARGET)
        find_library(FIBER_JEMALLOC_LIBRARY NAMES jemalloc)
        if (FIBER_JEMALLOC_LIBRARY)
            add_library(fiber_jemalloc UNKNOWN IMPORTED)
            set_target_properties(fiber_jemalloc PROPERTIES
                IMPORTED_LOCATION "${FIBER_JEMALLOC_LIBRARY}")
            set(FIBER_JEMALLOC_TARGET fiber_jemalloc)
        endif()
    endif()

    if (NOT FIBER_JEMALLOC_TARGET AND FIBER_FETCH_DEPS)
        fiber_prepare_jemalloc_target()
        if (TARGET fiber_jemalloc)
            set(FIBER_JEMALLOC_TARGET fiber_jemalloc)
        endif()
    endif()

    if (NOT FIBER_JEMALLOC_TARGET)
        message(FATAL_ERROR
            "FIBER_USE_JEMALLOC=ON but jemalloc was not found. "
            "Install jemalloc or enable FIBER_FETCH_DEPS=ON to download it automatically.")
    endif()

    target_link_libraries(fiber_allocator INTERFACE ${FIBER_JEMALLOC_TARGET})
    if (UNIX AND NOT APPLE)
        target_link_options(fiber_allocator INTERFACE -Wl,--no-as-needed)
    endif()
endif()

function(fiber_link_runtime_allocator target_name)
    target_link_libraries(${target_name} PRIVATE fiber_allocator)
    if (TARGET fiber_jemalloc_ep)
        add_dependencies(${target_name} fiber_jemalloc_ep)
    endif()
endfunction()

option(FIBER_ENABLE_LTO "Enable LTO/IPO for fiber targets" ON)
set(FIBER_IPO_SUPPORTED FALSE)
if (FIBER_ENABLE_LTO)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT FIBER_IPO_SUPPORTED OUTPUT FIBER_IPO_ERROR)
    if (FIBER_IPO_SUPPORTED)
        set_property(TARGET fiber_lib PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
    else()
        message(STATUS "IPO/LTO not supported: ${FIBER_IPO_ERROR}")
    endif()
endif()

file(GLOB EXAMPLE_SRC_LIST ${CMAKE_CURRENT_LIST_DIR}/example/*.cpp)
foreach(EXAMPLE_SRC IN LISTS EXAMPLE_SRC_LIST)
    get_filename_component(EXAMPLE_NAME ${EXAMPLE_SRC} NAME_WE)
    if (EXAMPLE_NAME STREQUAL "http3_demo_lsquic" AND NOT TARGET lsquic::lsquic)
        message(STATUS "Skipping example ${EXAMPLE_NAME} because lsquic is unavailable")
        continue()
    endif()
    add_executable(${EXAMPLE_NAME} ${EXAMPLE_SRC})
    if (EXAMPLE_NAME STREQUAL "http3_demo_lsquic")
        target_link_libraries(${EXAMPLE_NAME} PRIVATE lsquic::lsquic boringssl::ssl boringssl::crypto)
        if (TARGET ZLIB::ZLIB)
            target_link_libraries(${EXAMPLE_NAME} PRIVATE ZLIB::ZLIB)
        else()
            target_link_libraries(${EXAMPLE_NAME} PRIVATE z)
        endif()
        if (DEFINED lsquic_SOURCE_DIR)
            target_include_directories(${EXAMPLE_NAME} PRIVATE ${lsquic_SOURCE_DIR}/include)
        else()
            target_include_directories(${EXAMPLE_NAME} PRIVATE ${CMAKE_CURRENT_LIST_DIR}/temp/_deps/lsquic-src/include)
        endif()
        if (FIBER_USE_LIBCXX)
            target_link_options(${EXAMPLE_NAME} PRIVATE ${FIBER_STDLIB_LINK_FLAGS})
        endif()
    else()
        target_link_libraries(${EXAMPLE_NAME} PRIVATE fiber_lib)
    endif()
    fiber_link_runtime_allocator(${EXAMPLE_NAME})
    if (FIBER_ENABLE_LTO AND FIBER_IPO_SUPPORTED)
        set_property(TARGET ${EXAMPLE_NAME} PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
    endif()
endforeach()

option(FIBER_BUILD_TESTS "Build tests" ON)
if(FIBER_BUILD_TESTS)
    find_package(GTest QUIET)
    if (NOT GTest_FOUND)
        if (FIBER_FETCH_DEPS)
            include(FetchContent)
            set(FETCHCONTENT_UPDATES_DISCONNECTED ON)
            fiber_use_cached_content(googletest)
            FetchContent_Declare(
                googletest
                URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
            )
            set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
            FetchContent_MakeAvailable(googletest)
        else()
            message(STATUS "GoogleTest not found. Set FIBER_FETCH_DEPS=ON or install GTest to build tests.")
        endif()
    endif()
    if (GTest_FOUND OR TARGET GTest::gtest_main)
        if (CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
            if (TARGET gtest)
                target_compile_options(gtest PRIVATE -Wno-character-conversion)
            endif()
            if (TARGET gtest_main)
                target_compile_options(gtest_main PRIVATE -Wno-character-conversion)
            endif()
        endif()

        enable_testing()
        add_executable(fiber_tests
            tests/ArrayTest.cpp
            tests/SanityTest.cpp
            tests/GeneratorTest.cpp
            tests/IteratorTest.cpp
            tests/IoBufTest.cpp
            tests/JsValueOpsTest.cpp
            tests/JsValueEncodeTest.cpp
            tests/ParserTest.cpp
            tests/ScriptParserTest.cpp
            tests/ScriptCompilerTest.cpp
            tests/ScriptRuntimeOpsTest.cpp
            tests/ScriptExecutionTest.cpp
            tests/ScriptPlanTest.cpp
            tests/ThreadGroupTest.cpp
            tests/EventLoopTest.cpp
            tests/WaitGroupTest.cpp
            tests/SleepTest.cpp
            tests/TimeoutTest.cpp
            tests/MutexTest.cpp
            tests/SignalTest.cpp
            tests/TcpListenerTest.cpp
            tests/TcpStreamTest.cpp
            tests/UnixStreamTest.cpp
            tests/UdpSocketTest.cpp
            tests/IpAddressTest.cpp
            tests/Http1ServerTest.cpp
        )
        target_link_libraries(fiber_tests PRIVATE fiber_lib GTest::gtest_main)
        fiber_link_runtime_allocator(fiber_tests)
        if (FIBER_ENABLE_LTO AND FIBER_IPO_SUPPORTED)
            set_property(TARGET fiber_tests PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
        endif()
        include(GoogleTest)
        gtest_discover_tests(fiber_tests)
    endif()
endif()
