cmake_minimum_required(VERSION 3.24)

set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS version")

project(Splittr VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_subdirectory(third_party/JUCE)
include(cmake/DemucsCpp.cmake)

# --- Bundled assets (fonts) ---
juce_add_binary_data(SplittrAssets SOURCES
    assets/fonts/DMMono-Regular.ttf
    assets/fonts/DMMono-Medium.ttf)

# --- The plugin ---
juce_add_plugin(Splittr
    PRODUCT_NAME "Splittr"
    COMPANY_NAME "Brynjar"
    BUNDLE_ID com.brynjar.splittr
    PLUGIN_MANUFACTURER_CODE Brnj
    PLUGIN_CODE Spl1
    FORMATS AU VST3 Standalone
    AU_MAIN_TYPE kAudioUnitType_Effect
    VST3_CATEGORIES Fx Tools
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    COPY_PLUGIN_AFTER_BUILD TRUE)

target_sources(Splittr PRIVATE
    src/PluginProcessor.cpp
    src/PluginEditor.cpp
    src/ui/HeaderBar.cpp
    src/ui/MatrixBar.cpp
    src/ui/IdleDropView.cpp
    src/ui/DownloadView.cpp
    src/ui/WorkingView.cpp
    src/ui/StemRowComponent.cpp
    src/ui/DoneView.cpp
    src/engine/AudioFileIO.cpp
    src/engine/DemucsSeparator.cpp
    src/engine/SeparationEngine.cpp
    src/engine/ModelManager.cpp
    src/engine/StemCache.cpp
    src/engine/AuditionPlayer.cpp)

target_include_directories(Splittr PRIVATE src)

target_compile_definitions(Splittr PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_DISPLAY_SPLASH_SCREEN=0)

target_link_libraries(Splittr PRIVATE
    SplittrAssets
    demucs_cpp
    juce::juce_audio_utils
    juce::juce_audio_formats
    juce::juce_gui_extra
    juce::juce_recommended_config_flags
    juce::juce_recommended_lto_flags
    juce::juce_recommended_warning_flags)

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
