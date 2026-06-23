#!/usr/bin/env bash
# Builds the offscreen triangle milestone (pure C++ / metal-cpp).
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
v2="$here/../.."

clang++ -std=c++17 -fno-objc-arc -O2 \
  -I"$v2/third_party/metal-cpp" \
  -framework Metal -framework Foundation -framework QuartzCore \
  "$here/triangle.cpp" \
  "$here/metal_device.cpp" \
  "$here/metal_cpp_impl.cpp" \
  -o "$here/triangle"

echo "built $here/triangle"
