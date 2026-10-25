#!/bin/sh
# Compile glove.ino against the stubs in this folder and run the checks.
set -e
cd "$(dirname "$0")"
CXX=${CXX:-c++}
$CXX -std=gnu++17 -Wall -Wno-unused-function -I . sim.cpp -o /tmp/glove-sim
/tmp/glove-sim
