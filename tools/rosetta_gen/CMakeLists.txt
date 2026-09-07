# Standalone build of the rosetta_gen framework tool.
#
# From the repo root:
#   cmake -G Ninja -S tools/rosetta_gen -B tools/rosetta_gen/build
#   cmake --build tools/rosetta_gen/build
#   ./bin/rosetta_gen examples/generate/manifest.json
#
# The built binary is placed in <repo>/bin (not the build tree).
#
# This tool reads manifest.json and emits a per-project tool source
# (<generator_name>.cpp + CMakeLists.txt + bindings.h). Uses the system C++
# compiler — no clang-p2996 fork needed (this tool does not use
# reflection, only JSON parsing and text templating).

cmake_minimum_required(VERSION 3.28)

project(rosetta_gen CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Single-header release (≈900KB) instead of cloning the full repo
# (which carries ~200MB of test data).
include(FetchContent)
FetchContent_Declare(json
    URL      https://github.com/nlohmann/json/releases/download/v3.11.3/include.zip
    URL_HASH SHA256=a22461d13119ac5c78f205d3df1db13403e58ce1bb1794edc9313677313f4a9d)
FetchContent_MakeAvailable(json)

# Drop the binary in <repo>/bin rather than the build tree.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/../../bin)

add_executable(rosetta_gen
    rosetta_gen.cpp # main() + help text
    manifest.cpp    # manifest.json -> Manifest (load + glob expansion)
    doccomments.cpp # Doxygen / default-argument harvesting from the headers
    emit.cpp        # generator-project emission (bindings.h / driver / CMake)
    init.cpp        # --init (example manifest + heuristic source scan)
    build.cpp       # --build (the whole pipeline in one command)
    clean.cpp       # --clean
    util.cpp)       # file I/O + portable command running
target_include_directories(rosetta_gen PRIVATE ${json_SOURCE_DIR}/include)
