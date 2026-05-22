cmake_minimum_required(VERSION 3.20)
project(hft LANGUAGES CXX)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ─── Global compile settings ───────────────────────────────────────────
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# HFT-oriented flags. -march=native lets the compiler use your CPU's
# full instruction set (AVX, BMI, etc.).
add_compile_options(
  -Wall -Wextra -Wpedantic
  -march=native
)

# ─── The core library (header-only for now) ────────────────────────────
add_library(hft_core INTERFACE)
target_include_directories(hft_core INTERFACE
  ${CMAKE_SOURCE_DIR}/include
)
target_compile_features(hft_core INTERFACE cxx_std_20)

# ─── GoogleTest (system package) ───────────────────────────────────────
find_package(GTest REQUIRED)

# ─── Production binary ─────────────────────────────────────────────────
add_subdirectory(src)

# ─── Tools (exchange simulator, bench) ─────────────────────────────────
add_subdirectory(tools)

# ─── Tests ─────────────────────────────────────────────────────────────
enable_testing()
add_subdirectory(tests)