# Builds demucs.cpp inference code as a static library.
#
# Deliberately does NOT use third_party/demucs.cpp/CMakeLists.txt: upstream
# hard-requires OpenMP and compiles with -Ofast -march=native, neither of
# which works cleanly with Apple clang on arm64. We compile its sources with
# -O3 and route Eigen's GEMM through Apple Accelerate instead.

set(DEMUCS_CPP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/demucs.cpp")

file(GLOB DEMUCS_CPP_SOURCES CONFIGURE_DEPENDS "${DEMUCS_CPP_DIR}/src/*.cpp")

add_library(demucs_cpp STATIC ${DEMUCS_CPP_SOURCES})

target_include_directories(demucs_cpp SYSTEM PUBLIC
    "${DEMUCS_CPP_DIR}/src"
    "${DEMUCS_CPP_DIR}/vendor/eigen")

target_compile_features(demucs_cpp PUBLIC cxx_std_17)
target_compile_options(demucs_cpp PRIVATE -O3)

if(APPLE)
    target_compile_definitions(demucs_cpp PRIVATE EIGEN_USE_BLAS)
    target_link_libraries(demucs_cpp PUBLIC "-framework Accelerate")
endif()
