#!/bin/sh
# Build and run the write-path tests. Run from the repository root or from here.
set -e
cd "$(dirname "$0")/.."
g++ -std=c++17 -Wall -Wextra -Wno-unused-parameter \
    -I tests/stub -I components/dietrich \
    -o tests/test_write tests/test_write.cpp components/dietrich/dietrich.cpp
./tests/test_write
