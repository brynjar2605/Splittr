# Builds the vendored Basic Pitch transcription core as a static library.
#
# Like cmake/DemucsCpp.cmake this deliberately ignores the upstream build
# files: RTNeural ships its own CMakeLists that wants xsimd and a bundled
# Eigen, and NeuralNote's expects an ONNX Runtime static library and JUCE's
# BinaryData. We compile the handful of sources we actually need instead, on
# Eigen we already vendor for demucs.cpp.
#
# Everything here is SYSTEM-included so RTNeural's and nlohmann's headers stay
# out of juce_recommended_warning_flags.

set(BASIC_PITCH_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/basicpitch")

add_library(basicpitch STATIC
    "${BASIC_PITCH_DIR}/BasicPitch.cpp"
    "${BASIC_PITCH_DIR}/BasicPitchCNN.cpp"
    "${BASIC_PITCH_DIR}/Features.cpp"
    "${BASIC_PITCH_DIR}/Notes.cpp")

target_include_directories(basicpitch SYSTEM PUBLIC "${BASIC_PITCH_DIR}")

# RTNeural's model_loader.h includes "../modules/json/json.hpp" relative to
# itself, so json.hpp keeps the layout it has upstream and RTNeural stays
# byte-identical.
target_include_directories(basicpitch SYSTEM PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/demucs.cpp/vendor/eigen")

target_compile_features(basicpitch PUBLIC cxx_std_17)

# RTNeural's Eigen backend, and the alignment its CMake picks for arm64 NEON.
# Without RTNEURAL_DEFAULT_ALIGNMENT its config.h emits a #warning and guesses.
target_compile_definitions(basicpitch PRIVATE
    RTNEURAL_USE_EIGEN=1
    RTNEURAL_DEFAULT_ALIGNMENT=16
    RTNEURAL_NAMESPACE=RTNeural)

# The CNN is templated end to end; -O3 is the difference between a stem taking
# seconds and taking minutes. Deliberately not -Ofast: the note thresholds are
# compared against model output, and fast-math would drift them.
target_compile_options(basicpitch PRIVATE -O3)

# nlohmann's single header still writes `operator "" _json` with the space.
# It is vendored code, so silence it rather than patch it.
target_compile_options(basicpitch PRIVATE
    -Wno-unknown-warning-option -Wno-deprecated-literal-operator)
