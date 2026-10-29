#!/usr/bin/env bash

# grab version from CMake file with a slightly cursed sed call
cd "$(dirname "$0")/.."
echo "P64_VERSION=$(sed -nE 's/^project\(pyrite64 VERSION ([0-9.]+)\).*/\1/p' CMakeLists.txt | tr . -)"
