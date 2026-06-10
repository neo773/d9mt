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

echo "[build] compiling d3d9.dll (i686 PE, with in-process shader translation)"
V="$ROOT/vendor/dxvk"
SC="$ROOT/vendor/spirv-cross"
VENDOR_SOURCES=(
  # dxso translator, verbatim DXVK (dxso_options.cpp excluded: its ctor is
  # device-coupled; d9mt_translate.cpp provides the default one)
  "$V"/src/dxso/dxso_analysis.cpp
  "$V"/src/dxso/dxso_code.cpp
  "$V"/src/dxso/dxso_common.cpp
  "$V"/src/dxso/dxso_enums.cpp
  "$V"/src/dxso/dxso_compiler.cpp
  "$V"/src/dxso/dxso_ctab.cpp
  "$V"/src/dxso/dxso_decoder.cpp
  "$V"/src/dxso/dxso_header.cpp
  "$V"/src/dxso/dxso_module.cpp
  "$V"/src/dxso/dxso_reader.cpp
  "$V"/src/dxso/dxso_tables.cpp
  "$V"/src/dxso/dxso_util.cpp
  "$V"/src/spirv/spirv_code_buffer.cpp
  "$V"/src/spirv/spirv_compression.cpp
  "$V"/src/spirv/spirv_module.cpp
  "$V"/src/dxvk/dxvk_shader.cpp
  "$V"/src/dxvk/dxvk_pipelayout.cpp
  "$V"/src/dxvk/dxvk_shader_key.cpp
  "$V"/src/d3d9/d3d9_fixed_function.cpp
  "$V"/src/util/log/log.cpp
  "$V"/src/util/log/log_debug.cpp
  "$V"/src/util/util_env.cpp
  "$V"/src/util/util_matrix.cpp
  "$V"/src/util/util_string.cpp
  "$V"/src/util/thread.cpp
  "$V"/src/util/sha1/sha1_util.cpp
  "$V"/src/util/sha1/sha1.c
  "$V"/src/util/sync/sync_recursive.cpp
  # SPIRV-Cross, verbatim Khronos
  "$SC"/spirv_cross.cpp
  "$SC"/spirv_parser.cpp
  "$SC"/spirv_cross_parsed_ir.cpp
  "$SC"/spirv_cfg.cpp
  "$SC"/spirv_glsl.cpp
  "$SC"/spirv_msl.cpp
)
# gc-sections drops the unreferenced DXVK device/pipeline paths (the host
# build relies on -dead_strip the same way). PE ld resolves symbols before
# garbage collection, so d9mt_dxvk_stubs.cpp satisfies the dead paths'
# references into the non-vendored DXVK runtime.
i686-w64-mingw32-g++ -std=c++17 -O2 -shared -w \
  -ffunction-sections -fdata-sections -Wl,--gc-sections \
  -o "$BUILD/d3d9.dll" \
  "$ROOT/src/d3d9/d3d9.cpp" \
  "$ROOT/src/d3d9/d9mt_translate.cpp" \
  "$ROOT/src/d3d9/d9mt_dxvk_stubs.cpp" \
  "${VENDOR_SOURCES[@]}" \
  "$ROOT/src/d3d9/d3d9.def" \
  -I "$BUILD" \
  -I "$V/include/vulkan/include" \
  -I "$V/include/spirv/include" \
  -I "$V/src" \
  -I "$V/src/dxvk" \
  -DNOMINMAX \
  -L "$ROOT/prebuilt" -lwinemetal \
  -L "$BUILD/d9mtmetal" -ld9mtmetal32 \
  -luuid \
  -static -static-libgcc -static-libstdc++ \
  -Wl,--enable-stdcall-fixup

echo "[build] compiling triangle.exe (i686 PE)"
i686-w64-mingw32-gcc -O2 \
  -o "$BUILD/triangle.exe" \
  "$ROOT/test/triangle.c" \
  -ld3d9 -luser32 -lgdi32

# shadertri needs SM3 bytecode produced by hlsl2dxso.exe under wine
# (test/shadertri_*.hlsl); skipped if the blobs are absent
if [[ -f "$BUILD/shadertri_vs.vso" && -f "$BUILD/shadertri_ps.pso" ]]; then
  echo "[build] embedding shadertri bytecode + compiling shadertri.exe"
  python3 - "$BUILD/shadertri_vs.vso" "$BUILD/shadertri_vs_bytecode.h" shadertri_vs_bytecode <<'EOF'
import sys
data = open(sys.argv[1], 'rb').read()
with open(sys.argv[2], 'w') as f:
    f.write(f"static const unsigned char {sys.argv[3]}[] = {{\n")
    for i in range(0, len(data), 16):
        f.write("  " + ",".join(str(b) for b in data[i:i+16]) + ",\n")
    f.write("};\n")
EOF
  python3 - "$BUILD/shadertri_ps.pso" "$BUILD/shadertri_ps_bytecode.h" shadertri_ps_bytecode <<'EOF'
import sys
data = open(sys.argv[1], 'rb').read()
with open(sys.argv[2], 'w') as f:
    f.write(f"static const unsigned char {sys.argv[3]}[] = {{\n")
    for i in range(0, len(data), 16):
        f.write("  " + ",".join(str(b) for b in data[i:i+16]) + ",\n")
    f.write("};\n")
EOF
  i686-w64-mingw32-gcc -O2 \
    -o "$BUILD/shadertri.exe" \
    "$ROOT/test/shadertri.c" \
    -I "$BUILD" \
    -ld3d9 -luser32 -lgdi32
fi

echo "[build] done:"
ls -la "$BUILD/d3d9.dll" "$BUILD/triangle.exe"
