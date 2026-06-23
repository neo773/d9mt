#!/usr/bin/env bash
# Build the v2 minimal D3D9->Metal driver (d3d9.dll) + the code.exe triangle
# test app, both as i686 PE for Wine. Metal is reached only through the
# winemetal ABI (vendored, as-is from DXMT) + the d9mtmetal companion — no
# Vulkan, no metal-cpp (that is host-only tooling).
set -euo pipefail
V2="$(cd "$(dirname "$0")/.." && pwd)"
ROOT="$(cd "$V2/.." && pwd)"
BUILD="$V2/build"
mkdir -p "$BUILD"

MINGW=i686-w64-mingw32
WINEMETAL="$V2/third_party/winemetal"
D9MTMETAL="$ROOT/build/d9mtmetal"   # prebuilt import lib for the companion

echo "[v2] shader.metal -> metallib"
xcrun -sdk macosx metal -o "$BUILD/triangle.metallib" "$V2/d9mt/shaders/triangle.metal"

echo "[v2] embedding metallib -> triangle_metallib.h"
python3 - "$BUILD/triangle.metallib" "$BUILD/triangle_metallib.h" <<'EOF'
import sys
data = open(sys.argv[1], 'rb').read()
with open(sys.argv[2], 'w') as f:
    f.write("// generated from triangle.metallib by v2/scripts/build.sh\n")
    f.write("static const unsigned char d9mt_triangle_metallib[] = {\n")
    for i in range(0, len(data), 16):
        f.write("  " + ",".join(str(b) for b in data[i:i+16]) + ",\n")
    f.write("};\n")
    f.write(f"static const unsigned int d9mt_triangle_metallib_len = {len(data)};\n")
EOF

echo "[v2] d3d9.dll (i686 PE)"
"$MINGW-g++" -std=c++17 -O2 -shared -w \
  -ffunction-sections -fdata-sections -Wl,--gc-sections \
  -o "$BUILD/d3d9.dll" \
  "$V2/d9mt/api/d3d9_entry.cpp" \
  "$V2/d9mt/api/d3d9_device.cpp" \
  "$V2/d9mt/metal/metal_backend.cpp" \
  "$V2/d9mt/api/d3d9.def" \
  -I "$BUILD" \
  -I "$WINEMETAL" \
  -I "$V2/third_party/d9mtmetal" \
  -DNOMINMAX \
  -L "$WINEMETAL" -lwinemetal \
  -L "$D9MTMETAL" -ld9mtmetal32 \
  -luuid \
  -static -static-libgcc -static-libstdc++ \
  -Wl,--enable-stdcall-fixup

echo "[v2] code.exe (i686 PE)"
"$MINGW-gcc" -O2 -o "$BUILD/code.exe" "$V2/test/code.c" \
  -ld3d9 -luser32 -lgdi32

echo "[v2] done:"
ls -la "$BUILD/d3d9.dll" "$BUILD/code.exe"
