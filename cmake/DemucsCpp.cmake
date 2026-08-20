# Builds demucs.cpp inference code as a static library.
#
# Deliberately does NOT use third_party/demucs.cpp/CMakeLists.txt: upstream
# hard-requires OpenMP and compiles with -Ofast -march=native, neither of
# which works cleanly with Apple clang on arm64. We compile its sources with
# -O3 and route Eigen's GEMM through Apple Accelerate instead.

set(DEMUCS_CPP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/demucs.cpp")
set(DEMUCS_OVERRIDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/demucs-overrides")

file(GLOB DEMUCS_CPP_SOURCES CONFIGURE_DEPENDS "${DEMUCS_CPP_DIR}/src/*.cpp")

# demucs.cpp is a submodule we keep pristine, but some of its translation units
# are the per-thread ceiling (issue #24) and need rewriting. So a source in
# third_party/demucs-overrides/ replaces the upstream file of the same name:
# the submodule stays untouched and `git diff` shows the rewrite as our code.
# See that directory's README.md for what is overridden and why.
foreach(override IN LISTS DEMUCS_CPP_SOURCES)
    get_filename_component(override_name "${override}" NAME)
    if(EXISTS "${DEMUCS_OVERRIDE_DIR}/${override_name}")
        list(REMOVE_ITEM DEMUCS_CPP_SOURCES "${override}")
        list(APPEND DEMUCS_CPP_SOURCES "${DEMUCS_OVERRIDE_DIR}/${override_name}")
    endif()
endforeach()

add_library(demucs_cpp STATIC ${DEMUCS_CPP_SOURCES})

# Overrides first so a header override would also win over upstream's.
target_include_directories(demucs_cpp SYSTEM PUBLIC
    "${DEMUCS_OVERRIDE_DIR}"
    "${DEMUCS_CPP_DIR}/src"
    "${DEMUCS_CPP_DIR}/vendor/eigen")

target_compile_features(demucs_cpp PUBLIC cxx_std_17)

# -O3 and not upstream's -Ofast, but for a different reason than in
# cmake/BasicPitch.cmake: there fast-math is refused because it would drift
# note thresholds, here it was measured and it is simply slower. Both
# -ffast-math and -ffast-math -fno-finite-math-only stayed accurate (>100 dB
# per-stem SNR, near-silent input still sane) and both cost ~28% wall time.
target_compile_options(demucs_cpp PRIVATE -O3)

if(APPLE)
    target_compile_definitions(demucs_cpp PRIVATE EIGEN_USE_BLAS)
    target_link_libraries(demucs_cpp PUBLIC "-framework Accelerate")
endif()
