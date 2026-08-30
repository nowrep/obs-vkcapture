#!/bin/bash
set -e

# Define build directory
BUILD_DIR="build"

echo "=== OBS-VKCAPTURE BUILD SCRIPT ==="
echo "Configuring CMake project..."
cmake -B "$BUILD_DIR" -S . -DCMAKE_BUILD_TYPE=Release

echo "Compiling targets..."
cmake --build "$BUILD_DIR" --parallel $(nproc)

echo "=== BUILD SUCCESSFUL ==="
echo "Artifacts are compiled under the '$BUILD_DIR' directory."
