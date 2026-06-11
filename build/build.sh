#!/bin/bash
# OMNIS - basic build. For PGO, use build_pgo.sh. For CMake, see CMakeLists.txt.
set -e
cd "$(dirname "$0")"

echo "Building omnis (engine + CLI)..."
g++ -std=c++17 -O3 -march=native -I../src ../src/cli.cpp -o omnis

echo "Done.  Try: echo 'demo 4 8 0 1 2 3 0 1 2 3' | ./omnis -"
