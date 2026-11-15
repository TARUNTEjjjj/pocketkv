cmake_minimum_required(VERSION 3.20)
project(PocketKV VERSION 0.1.0 LANGUAGES CXX)

if(NOT CMAKE_SYSTEM_NAME MATCHES "^(Linux|Darwin)$")
    message(FATAL_ERROR "PocketKV currently supports Linux and macOS (POSIX file I/O and flock).")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
include(GNUInstallDirs)
option(POCKETKV_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(POCKETKV_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
option(POCKETKV_BUILD_BENCHMARKS "Build the reproducible storage benchmark" OFF)

function(pocketkv_configure target)
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow)
    if(POCKETKV_WARNINGS_AS_ERRORS)
        target_compile_options(${target} PRIVATE -Werror)
    endif()
    if(POCKETKV_SANITIZERS)
        target_compile_options(${target} PRIVATE -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer)
        target_link_options(${target} PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
    endif()
endfunction()

add_library(pocketkv_lib STATIC src/store.cpp src/format.cpp)
add_library(PocketKV::pocketkv ALIAS pocketkv_lib)
set_target_properties(pocketkv_lib PROPERTIES OUTPUT_NAME pocketkv EXPORT_NAME pocketkv)
target_include_directories(pocketkv_lib PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_compile_features(pocketkv_lib PUBLIC cxx_std_20)
pocketkv_configure(pocketkv_lib)

add_executable(pocketkv app/main.cpp)
set_target_properties(pocketkv PROPERTIES EXPORT_NAME cli)
target_link_libraries(pocketkv PRIVATE PocketKV::pocketkv)
pocketkv_configure(pocketkv)
add_executable(pocketkv-example examples/basic.cpp)
target_link_libraries(pocketkv-example PRIVATE PocketKV::pocketkv)
pocketkv_configure(pocketkv-example)

if(POCKETKV_BUILD_BENCHMARKS)
    add_executable(pocketkv-benchmark benchmarks/storage_benchmark.cpp)
    target_link_libraries(pocketkv-benchmark PRIVATE PocketKV::pocketkv)
    pocketkv_configure(pocketkv-benchmark)
endif()

include(CTest)
if(BUILD_TESTING)
    add_executable(pocketkv-tests tests/test_store.cpp)
    target_include_directories(pocketkv-tests PRIVATE src)
    target_link_libraries(pocketkv-tests PRIVATE PocketKV::pocketkv)
    pocketkv_configure(pocketkv-tests)
    add_test(NAME storage-core COMMAND pocketkv-tests)
    set_tests_properties(storage-core PROPERTIES TIMEOUT 120)

    add_executable(pocketkv-page-tests tests/test_key_pages.cpp)
    target_link_libraries(pocketkv-page-tests PRIVATE PocketKV::pocketkv)
    pocketkv_configure(pocketkv-page-tests)
    add_test(NAME key-pages COMMAND pocketkv-page-tests)
    set_tests_properties(key-pages PROPERTIES TIMEOUT 120)

    add_executable(pocketkv-snapshot-tests tests/test_snapshots.cpp)
    target_link_libraries(pocketkv-snapshot-tests PRIVATE PocketKV::pocketkv)
    pocketkv_configure(pocketkv-snapshot-tests)
    add_test(NAME snapshots COMMAND pocketkv-snapshot-tests)
    set_tests_properties(snapshots PROPERTIES TIMEOUT 120)

    # The shim loads system headers before substituting Store calls, so fortified
    # inline libc wrappers cannot bypass the test hooks. Production is unchanged.
    add_library(pocketkv_fault_store OBJECT src/store.cpp)
    target_include_directories(pocketkv_fault_store PRIVATE include)
    target_compile_options(pocketkv_fault_store PRIVATE
        -include "${CMAKE_CURRENT_SOURCE_DIR}/tests/fault_syscalls.hpp")
    pocketkv_configure(pocketkv_fault_store)
    add_executable(pocketkv-fault-tests tests/test_faults.cpp src/format.cpp)
    target_include_directories(pocketkv-fault-tests PRIVATE include)
    target_link_libraries(pocketkv-fault-tests PRIVATE pocketkv_fault_store)
    pocketkv_configure(pocketkv-fault-tests)
    add_test(NAME storage-faults COMMAND pocketkv-fault-tests)
    set_tests_properties(storage-faults PROPERTIES TIMEOUT 120)

    add_test(NAME cli-help COMMAND pocketkv --help)
    find_package(Python3 3.9 COMPONENTS Interpreter)
    if(Python3_Interpreter_FOUND)
        # A CLI build whose large allocations fail checks that running out of
        # memory is reported, never printed as partial output. Sanitizer builds
        # skip it: AddressSanitizer brings its own operator new.
        set(pocketkv_cli_test_options)
        if(NOT POCKETKV_SANITIZERS)
            add_executable(pocketkv-limited-cli app/main.cpp tests/allocation_limit.cpp)
            target_link_libraries(pocketkv-limited-cli PRIVATE PocketKV::pocketkv)
            pocketkv_configure(pocketkv-limited-cli)
            set(pocketkv_cli_test_options --allocation-limited $<TARGET_FILE:pocketkv-limited-cli>)
        endif()
        add_test(NAME cli-workflows COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_test.py
            $<TARGET_FILE:pocketkv> ${pocketkv_cli_test_options})
        set_tests_properties(cli-workflows PROPERTIES TIMEOUT 120)
        add_executable(pocketkv-scenario-driver tests/scenario_driver.cpp)
        target_link_libraries(pocketkv-scenario-driver PRIVATE PocketKV::pocketkv)
        pocketkv_configure(pocketkv-scenario-driver)
        add_test(NAME real-world-scenarios COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/scenarios_test.py
            $<TARGET_FILE:pocketkv> $<TARGET_FILE:pocketkv-scenario-driver>)
        set_tests_properties(real-world-scenarios PROPERTIES TIMEOUT 120)
        if(POCKETKV_BUILD_BENCHMARKS)
            add_test(NAME benchmark-accounting COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/benchmark_test.py $<TARGET_FILE:pocketkv-benchmark>)
            set_tests_properties(benchmark-accounting PROPERTIES TIMEOUT 120)
        endif()
    else()
        message(STATUS "Python 3.9+ unavailable: optional CLI workflow tests will not run.")
    endif()
    if(NOT IS_ABSOLUTE "${CMAKE_INSTALL_BINDIR}" AND
       NOT IS_ABSOLUTE "${CMAKE_INSTALL_LIBDIR}" AND
       NOT IS_ABSOLUTE "${CMAKE_INSTALL_INCLUDEDIR}")
        add_test(NAME installed-consumer COMMAND ${CMAKE_COMMAND}
            -DPOCKET_BUILD_DIR=${CMAKE_CURRENT_BINARY_DIR}
            -DPOCKET_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
            -DPOCKET_COMPILER=${CMAKE_CXX_COMPILER}
            -DPOCKET_CONFIG=$<CONFIG>
            -DPOCKET_BINDIR=${CMAKE_INSTALL_BINDIR}
            -DPOCKET_LIBDIR=${CMAKE_INSTALL_LIBDIR}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/installed_consumer.cmake)
        set_tests_properties(installed-consumer PROPERTIES TIMEOUT 120)
    else()
        message(STATUS "Isolated installation test requires relative install directories; skipping it for absolute destinations.")
    endif()
endif()

include(CMakePackageConfigHelpers)
install(TARGETS pocketkv_lib pocketkv EXPORT PocketKVTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR} RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT PocketKVTargets FILE PocketKVTargets.cmake NAMESPACE PocketKV:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/PocketKV)
configure_package_config_file(cmake/PocketKVConfig.cmake.in ${CMAKE_CURRENT_BINARY_DIR}/PocketKVConfig.cmake INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/PocketKV)
write_basic_package_version_file(${CMAKE_CURRENT_BINARY_DIR}/PocketKVConfigVersion.cmake VERSION ${PROJECT_VERSION} COMPATIBILITY SameMajorVersion)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/PocketKVConfig.cmake ${CMAKE_CURRENT_BINARY_DIR}/PocketKVConfigVersion.cmake DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/PocketKV)

