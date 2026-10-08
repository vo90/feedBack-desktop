cmake_minimum_required(VERSION 3.22)
project(native_backing_tests LANGUAGES C CXX)
# Standalone no-device test build; reuse an explicitly supplied pinned JUCE tree.
# Never download/install dependencies or load an ASIO driver to run these tests.
if(NOT EXISTS "${JUCE_SOURCE_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "Supply -DJUCE_SOURCE_DIR=<existing pinned JUCE checkout>")
endif()
if(NOT EXISTS "${SIGNALSMITH_STRETCH_DIR}/signalsmith-stretch.h")
    message(FATAL_ERROR "Supply -DSIGNALSMITH_STRETCH_DIR=<existing pinned signalsmith-stretch checkout>")
endif()
add_subdirectory("${JUCE_SOURCE_DIR}" "${CMAKE_BINARY_DIR}/juce" EXCLUDE_FROM_ALL)
enable_testing()
add_subdirectory(../engine_units "${CMAKE_BINARY_DIR}/engine_units")
target_include_directories(backing_player_clock_test PRIVATE
    "${SIGNALSMITH_STRETCH_DIR}" "${SIGNALSMITH_STRETCH_DIR}/..")
