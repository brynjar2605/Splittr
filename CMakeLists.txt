cmake_minimum_required(VERSION 3.24)

set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS version")

project(Splittr VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_subdirectory(third_party/JUCE)
include(cmake/DemucsCpp.cmake)
include(cmake/BasicPitch.cmake)

# --- Bundled assets (fonts) ---
juce_add_binary_data(SplittrAssets SOURCES
    assets/fonts/DMMono-Regular.ttf
    assets/fonts/DMMono-Medium.ttf)

# --- Basic Pitch weights (~790 KB, Apache-2.0; see third_party/basicpitch) ---
# Small enough to live in the binary rather than go through ModelManager's
# download path, so MIDI extraction works on a fresh install with no network.
# Own namespace/header: two binary-data targets cannot both be BinaryData.h.
juce_add_binary_data(SplittrModels
    NAMESPACE BasicPitchWeights
    HEADER_NAME BasicPitchWeights.h
    SOURCES
        third_party/basicpitch/weights/cqt_kernels.bin
        third_party/basicpitch/weights/cnn_contour_model.json
        third_party/basicpitch/weights/cnn_note_model.json
        third_party/basicpitch/weights/cnn_onset_1_model.json
        third_party/basicpitch/weights/cnn_onset_2_model.json)

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
    ICON_BIG "${CMAKE_CURRENT_SOURCE_DIR}/assets/icon/splittr-icon.png"
    COPY_PLUGIN_AFTER_BUILD TRUE)

target_sources(Splittr PRIVATE
    src/PluginProcessor.cpp
    src/PluginEditor.cpp
    src/ui/Theme.cpp
    src/ui/HeaderBar.cpp
    src/ui/MatrixBar.cpp
    src/ui/IdleDropView.cpp
    src/ui/DownloadView.cpp
    src/ui/WorkingView.cpp
    src/ui/StemRowComponent.cpp
    src/ui/DoneView.cpp
    src/ui/ConfirmView.cpp
    src/ui/SettingsView.cpp
    src/ui/LibraryView.cpp
    src/engine/AudioFileIO.cpp
    src/engine/DemucsSeparator.cpp
    src/engine/MidiTranscriber.cpp
    src/engine/SeparationEngine.cpp
    src/engine/ModelManager.cpp
    src/engine/StemCache.cpp
    src/engine/StemFx.cpp
    src/engine/AuditionPlayer.cpp
    src/engine/TrackAnalyzer.cpp
    src/ui/TuneView.cpp)

target_include_directories(Splittr PRIVATE src)

target_compile_definitions(Splittr PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_DISPLAY_SPLASH_SCREEN=0)

target_link_libraries(Splittr PRIVATE
    SplittrAssets
    SplittrModels
    basicpitch
    demucs_cpp
    juce::juce_audio_utils
    juce::juce_audio_formats
    juce::juce_dsp
    juce::juce_gui_extra
    juce::juce_recommended_config_flags
    juce::juce_recommended_lto_flags
    juce::juce_recommended_warning_flags)

# --- Finder icon for the plugin bundles ---
# JUCE's ICON_BIG only applies to the Standalone app; stamp the same icns
# into the AU/VST3 bundles (build artefact + installed copy) and re-sign.
set(SPLITTR_ICNS "${CMAKE_CURRENT_SOURCE_DIR}/assets/icon/splittr.icns")

foreach(pair "AU;Components/Splittr.component" "VST3;VST3/Splittr.vst3")
    list(GET pair 0 fmt)
    list(GET pair 1 installed)
    set(installedBundle "$ENV{HOME}/Library/Audio/Plug-Ins/${installed}")

    add_custom_command(TARGET Splittr_${fmt} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy "${SPLITTR_ICNS}"
                "$<TARGET_BUNDLE_DIR:Splittr_${fmt}>/Contents/Resources/splittr.icns"
        COMMAND plutil -replace CFBundleIconFile -string splittr
                "$<TARGET_BUNDLE_DIR:Splittr_${fmt}>/Contents/Info.plist"
        COMMAND ${CMAKE_COMMAND} -E copy "${SPLITTR_ICNS}"
                "${installedBundle}/Contents/Resources/splittr.icns"
        COMMAND plutil -replace CFBundleIconFile -string splittr
                "${installedBundle}/Contents/Info.plist"
        COMMAND codesign --force -s - "${installedBundle}"
        VERBATIM)
endforeach()

# --- CLI test harness (M1): proves the separation engine before plugin work ---
juce_add_console_app(splittr_cli PRODUCT_NAME "splittr_cli")

target_sources(splittr_cli PRIVATE
    tools/splittr_cli/main.cpp
    src/engine/AudioFileIO.cpp
    src/engine/DemucsSeparator.cpp
    src/engine/MidiTranscriber.cpp
    src/engine/TrackAnalyzer.cpp)

target_include_directories(splittr_cli PRIVATE src)

target_compile_definitions(splittr_cli PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0)

target_link_libraries(splittr_cli PRIVATE
    SplittrModels
    basicpitch
    demucs_cpp
    juce::juce_audio_formats
    juce::juce_dsp
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags)

# --- Metal (MPSGraph) inference backend (issue #24) ---
# One ObjC++ TU, shared by the plugin and the CLI. It self-guards with
# #if defined(__APPLE__), and every entry point runtime-checks the OS/GPU, so
# adding it here is safe even though the deployment target predates MPSGraph
# features it uses. ARC because the engine holds Metal objects in C++ members
# and must survive C++ exceptions (cancellation) unwinding through it.
if(APPLE)
    enable_language(OBJCXX)

    set_target_properties(Splittr splittr_cli PROPERTIES
        OBJCXX_STANDARD 17
        OBJCXX_STANDARD_REQUIRED ON)

    target_sources(Splittr PRIVATE src/engine/DemucsMetal.mm)
    target_sources(splittr_cli PRIVATE src/engine/DemucsMetal.mm)

    set_source_files_properties(src/engine/DemucsMetal.mm PROPERTIES
        COMPILE_FLAGS "-fobjc-arc")

    foreach(metal_target Splittr splittr_cli)
        target_link_libraries(${metal_target} PRIVATE
            "-framework Metal"
            "-framework MetalPerformanceShadersGraph"
            "-framework Foundation")
    endforeach()
endif()
