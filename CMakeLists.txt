cmake_minimum_required(VERSION 3.24)

set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS version")

project(Splittr VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_subdirectory(third_party/JUCE)
include(cmake/DemucsCpp.cmake)

# --- Engine sources shared between the CLI and the plugin (no GUI deps) ---
set(STEMSPLIT_ENGINE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src/engine")

# --- CLI test harness (M1): proves the separation engine before plugin work ---
juce_add_console_app(splittr_cli PRODUCT_NAME "splittr_cli")

target_sources(splittr_cli PRIVATE
    tools/splittr_cli/main.cpp
    src/engine/AudioFileIO.cpp
    src/engine/DemucsSeparator.cpp)

target_include_directories(splittr_cli PRIVATE src)

target_compile_definitions(splittr_cli PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0)

target_link_libraries(splittr_cli PRIVATE
    demucs_cpp
    juce::juce_audio_formats
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags)
