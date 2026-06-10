#!/usr/bin/env bash
# Build dxso2spv: host-side (macOS native) D3D9 bytecode -> SPIR-V tool,
# compiling DXVK's vendored dxso translator with dxvk's own native shims.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
V="$ROOT/vendor/dxvk"
OUT="$ROOT/build"
mkdir -p "$OUT"

SOURCES=(
  # dxso translator, verbatim (dxso_options.cpp excluded: device-coupled
  # constructor; tools/shim.cpp provides the default constructor)
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
  # spirv module builder, verbatim
  "$V"/src/spirv/spirv_code_buffer.cpp
  "$V"/src/spirv/spirv_compression.cpp
  "$V"/src/spirv/spirv_module.cpp
  # dxvk shader wrapper + d3d9 helpers dxso links against
  "$V"/src/dxvk/dxvk_shader.cpp
  "$V"/src/dxvk/dxvk_pipelayout.cpp
  "$V"/src/dxvk/dxvk_shader_key.cpp
  "$V"/src/d3d9/d3d9_fixed_function.cpp
  # util subset
  "$V"/src/util/log/log.cpp
  "$V"/src/util/log/log_debug.cpp
  "$V"/src/util/util_env.cpp
  "$V"/src/util/util_matrix.cpp
  "$V"/src/util/util_string.cpp
  "$V"/src/util/thread.cpp
  "$V"/src/util/sha1/sha1_util.cpp
  "$V"/src/util/sha1/sha1.c
  "$V"/src/util/sync/sync_recursive.cpp
  # d9mt
  "$ROOT"/tools/shim.cpp
  "$ROOT"/tools/dxso2spv.cpp
)

clang++ -std=c++17 -O1 -w \
  -I "$V/include/native" \
  -I "$V/include/native/windows" \
  -I "$V/include/native/directx" \
  -I "$V/include/vulkan/include" \
  -I "$V/include/spirv/include" \
  -I "$V/src" \
  -I "$V/src/dxvk" \
  -DNOMINMAX \
  -Wl,-dead_strip \
  -o "$OUT/dxso2spv" \
  "${SOURCES[@]}"

echo "built $OUT/dxso2spv"
