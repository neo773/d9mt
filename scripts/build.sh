#!/usr/bin/env bash
# Build d9mt: shader.metal -> metallib -> embedded header, then the 32-bit
# d3d9.dll (mingw) and triangle.exe test app.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$ROOT/build"
mkdir -p "$BUILD"

echo "[build] compiling shader.metal -> metallib"
xcrun -sdk macosx metal -o "$BUILD/shader.metallib" "$ROOT/src/d3d9/shader.metal"

echo "[build] embedding metallib -> shader_metallib.h"
python3 - "$BUILD/shader.metallib" "$BUILD/shader_metallib.h" <<'EOF'
import sys
data = open(sys.argv[1], 'rb').read()
with open(sys.argv[2], 'w') as f:
    f.write("// generated from shader.metallib by build.sh\n")
    f.write("static const unsigned char d9mt_shader_metallib[] = {\n")
    for i in range(0, len(data), 16):
        f.write("  " + ",".join(str(b) for b in data[i:i+16]) + ",\n")
    f.write("};\n")
    f.write(f"static const unsigned int d9mt_shader_metallib_len = {len(data)};\n")
EOF

echo "[build] compiling d3d9.dll (i686 PE)"
i686-w64-mingw32-g++ -std=c++17 -O2 -shared \
  -o "$BUILD/d3d9.dll" \
  "$ROOT/src/d3d9/d3d9.cpp" \
  "$ROOT/src/d3d9/d3d9.def" \
  -I "$BUILD" \
  -L "$ROOT/prebuilt" -lwinemetal -luuid \
  -static -static-libgcc -static-libstdc++ \
  -Wl,--enable-stdcall-fixup

echo "[build] compiling triangle.exe (i686 PE)"
i686-w64-mingw32-gcc -O2 \
  -o "$BUILD/triangle.exe" \
  "$ROOT/test/triangle.c" \
  -ld3d9 -luser32 -lgdi32

echo "[build] done:"
ls -la "$BUILD/d3d9.dll" "$BUILD/triangle.exe"
