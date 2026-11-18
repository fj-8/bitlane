# SPDX-License-Identifier: MIT OR Apache-2.0
cmake_minimum_required(VERSION 3.21)

project(bitlane
        VERSION 0.4.0
        DESCRIPTION "Header-only compressed bitmap index for 32-bit integer sets"
        LANGUAGES CXX)

# Header-only by design: consuming projects add the include directory and link
# nothing. The namespaced alias mirrors what an installed package provides.
add_library(bitlane INTERFACE)
add_library(bitlane::bitlane ALIAS bitlane)

target_include_directories(bitlane INTERFACE
                           $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
                           $<INSTALL_INTERFACE:include>)
target_compile_features(bitlane INTERFACE cxx_std_20)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

install(DIRECTORY include/bitlane DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(TARGETS bitlane EXPORT bitlaneTargets)
install(EXPORT bitlaneTargets
        NAMESPACE bitlane::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/bitlane)
write_basic_package_version_file(
  "${CMAKE_CURRENT_BINARY_DIR}/bitlaneConfigVersion.cmake"
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion
  ARCH_INDEPENDENT)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/bitlaneConfigVersion.cmake"
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/bitlane)

# Without a config file, find_package(bitlane CONFIG) cannot locate the package
# even though the exported targets exist.
configure_package_config_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/cmake/bitlaneConfig.cmake.in"
  "${CMAKE_CURRENT_BINARY_DIR}/bitlaneConfig.cmake"
  INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/bitlane")
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/bitlaneConfig.cmake"
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/bitlane)

option(BITLANE_BUILD_TESTS "Build the assertion suite" ${PROJECT_IS_TOP_LEVEL})
option(BITLANE_BUILD_BENCH "Build the benchmark harness" ${PROJECT_IS_TOP_LEVEL})
option(BITLANE_BUILD_EXAMPLES "Build the examples" ${PROJECT_IS_TOP_LEVEL})

# Warnings are a development concern; consumers importing the header keep theirs.
if(BITLANE_BUILD_TESTS OR BITLANE_BUILD_BENCH OR BITLANE_BUILD_EXAMPLES)
  add_library(bitlane_warnings INTERFACE)
  if(MSVC)
    target_compile_options(bitlane_warnings INTERFACE /W4 /permissive- /utf-8 /EHsc)
  else()
    target_compile_options(bitlane_warnings INTERFACE
      -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion)
  endif()
endif()

if(BITLANE_BUILD_TESTS)
  enable_testing()
  add_executable(bitlane_tests tests/bitmap_tests.cpp)
  target_link_libraries(bitlane_tests PRIVATE bitlane::bitlane bitlane_warnings)
  target_include_directories(bitlane_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
  add_test(NAME bitlane_tests COMMAND bitlane_tests)
endif()

if(BITLANE_BUILD_BENCH)
  add_executable(bitlane_bench bench/bench.cpp)
  target_link_libraries(bitlane_bench PRIVATE bitlane::bitlane bitlane_warnings)
  if(NOT MSVC)
    target_compile_options(bitlane_bench PRIVATE -O2)
  endif()
endif()

if(BITLANE_BUILD_EXAMPLES)
  foreach(example posting_index tag_query)
    add_executable(example_${example} examples/${example}.cpp)
    target_link_libraries(example_${example} PRIVATE bitlane::bitlane bitlane_warnings)
  endforeach()
endif()
