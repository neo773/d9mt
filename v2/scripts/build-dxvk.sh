#!/usr/bin/env bash
# Build the REAL DXVK D3D9 frontend (v2/d3d9) against the Metal shim (v2/dxvk)
# into d3d9.dll, plus code.exe. Phase 1 target: compile + link + render one
# fixed-function triangle through the unmodified frontend. Metal via winemetal.
set -euo pipefail
V2="$(cd "$(dirname "$0")/.." && pwd)"
ROOT="$(cd "$V2/.." && pwd)"
BUILD="$V2/build"
mkdir -p "$BUILD"
MINGW=i686-w64-mingw32
WINEMETAL="$V2/third_party/winemetal"
D9MTMETAL="$ROOT/build/d9mtmetal"

echo "[dxvk] shader.metal -> embedded metallib"
xcrun -sdk macosx metal -o "$BUILD/triangle.metallib" "$V2/d9mt/shaders/triangle.metal"
python3 - "$BUILD/triangle.metallib" "$BUILD/triangle_metallib.h" <<'EOF'
import sys
data = open(sys.argv[1], 'rb').read()
with open(sys.argv[2], 'w') as f:
    f.write("static const unsigned char d9mt_triangle_metallib[] = {\n")
    for i in range(0, len(data), 16):
        f.write("  " + ",".join(str(b) for b in data[i:i+16]) + ",\n")
    f.write("};\n")
    f.write(f"static const unsigned int d9mt_triangle_metallib_len = {len(data)};\n")
EOF

# Fixed-function shaders: compile DXVK's GLSL to SPIR-V byte-array headers
# (d3d9_fixed_function_vert/frag[_sample].h), exactly what upstream's meson does.
# The shader-conversion module translates this real SPIR-V to MSL; the old
# v2/dxvk/shader_stubs 1-word placeholders are superseded by these (listed first).
FFSH="$BUILD/ff_shaders"; mkdir -p "$FFSH"
echo "[dxvk] fixed-function GLSL -> SPIR-V headers"
gen_ff() { # <stage-file> <array-name>
  local src="$V2/d3d9/shaders/$1"
  local name="$2"
  local out="$FFSH/$name.h"
  [[ -f "$out" && "$out" -nt "$src" ]] && return
  glslangValidator -V --target-env vulkan1.3 -I"$V2/d3d9/shaders" \
    --vn "$name" -o "$out" "$src"
}
gen_ff d3d9_fixed_function_vert.vert        d3d9_fixed_function_vert
gen_ff d3d9_fixed_function_frag.frag        d3d9_fixed_function_frag
gen_ff d3d9_fixed_function_frag_sample.frag d3d9_fixed_function_frag_sample

INC=(
  -I "$BUILD"
  -I "$FFSH"                          # real FF SPIR-V byte arrays (generated above)
  -I "$V2/d3d9" -I "$V2"
  -I "$ROOT/vendor/dxvk/generated"   # generated d3d9_convert_*.h SPIR-V byte arrays
  -I "$V2/dxvk/shader_stubs"         # stub FF arrays (fallback; FFSH wins)
  -I "$V2/third_party/khronos/vulkan/include"
  -I "$V2/third_party/khronos/spirv/include"
  -I "$V2/third_party/dxbc-spirv"
  -I "$WINEMETAL" -I "$V2/third_party/d9mtmetal"
  -I "$ROOT/vendor/spirv-cross"     # SPIR-V -> MSL (shader conversion module)
)

# Frontend + shim + the kept util sources. (Source list refined during the
# compile-convergence pass; start broad and prune what gc-sections drops.)
# util/com/com_guid.cpp is excluded: it includes ../../d3d11 + ../../dxgi
# interface headers that don't exist in this tree. Its two exported symbols
# (logQueryInterfaceError, operator<<(REFIID)) are provided by the shim stubs
# cpp instead, so we never pull the d3d11/dxgi machinery.
UTIL_SRCS=()
while IFS= read -r f; do
  case "$f" in
    */util/com/com_guid.cpp) ;;            # excluded (see above)
    *) UTIL_SRCS+=("$f") ;;
  esac
done < <(find "$V2/util" -name '*.cpp')

# dxbc-spirv shader translation lib (KEPT per plan: state machine + shader xlat).
# d3d9_shader.cpp / d3d9_fixed_function.cpp lower D3D9 bytecode through this IR.
# The shim ignores the *output* (no SPIR-V reaches Metal), but the frontend still
# runs translation, so the real ir/sm3/spirv/util sources must link. Tests, tools,
# and the dxbc (sm4/sm5) path are excluded — D3D9 only needs sm3.
# SPIRV-Cross (verbatim Khronos): SPIR-V -> MSL for the shader-conversion module
# (dxvk_shader_convert.cpp). Same TU set v1 used (scripts/build-dxvkfe.sh).
SC="$ROOT/vendor/spirv-cross"
SPIRV_CROSS_SRCS=(
  "$SC"/spirv_cross.cpp
  "$SC"/spirv_parser.cpp
  "$SC"/spirv_cross_parsed_ir.cpp
  "$SC"/spirv_cfg.cpp
  "$SC"/spirv_glsl.cpp
  "$SC"/spirv_msl.cpp
)

DXBC_SPV="$V2/third_party/dxbc-spirv"
SHADER_SRCS=(
  "$DXBC_SPV"/ir/*.cpp
  "$DXBC_SPV"/ir/passes/*.cpp
  "$DXBC_SPV"/sm3/*.cpp
  "$DXBC_SPV"/spirv/spirv_builder.cpp
  "$DXBC_SPV"/spirv/spirv_mapping.cpp
  "$DXBC_SPV"/util/util_float16.cpp
  "$DXBC_SPV"/util/util_log.cpp
  "$DXBC_SPV"/util/util_md5.cpp
  "$DXBC_SPV"/util/util_swizzle.cpp
)

SRCS=(
  "$V2"/d3d9/*.cpp
  "$V2"/dxvk/*.cpp
  "$V2"/spirv/spirv_code_buffer.cpp           # SpirvCodeBuffer (FF shader byte buffer; shim ignores output)
  "$V2"/util/sha1/sha1.c                       # SHA1Init/Update/Final (C; util_rc find globs only *.cpp)
  "${SHADER_SRCS[@]}"
  "${SPIRV_CROSS_SRCS[@]}"
  "${UTIL_SRCS[@]}"
)

# ---------------------------------------------------------------------------
# Compile: per-TU object cache, parallel, incremental. One monolithic g++ over
# ~110 TUs took minutes and recompiled everything every run; this compiles each
# source to its own .o, skips up-to-date ones, runs NJOBS at a time, and routes
# through sccache when present. Touching a shim header rebuilds only the v2 TUs.
# ---------------------------------------------------------------------------
OBJ="$BUILD/obj"; mkdir -p "$OBJ"
SCCACHE="$(command -v sccache || true)"   # transparent compile cache when available
CXX="$SCCACHE $MINGW-g++"
CC="$SCCACHE $MINGW-gcc"
NJOBS="$(sysctl -n hw.ncpu 2>/dev/null || echo 8)"

COMMON=(-O1 -w -msse2 -ffunction-sections -fdata-sections -DNOMINMAX "${INC[@]}")
# v2 frontend/shim/util/dxbc-spirv: force_inline (util_likely.h) must be visible
# before util_rc_ptr.h uses it at class scope, so inject it globally for these.
V2FLAGS=(-std=c++20 "${COMMON[@]}" -Wno-template-body -include "$V2/util/util_likely.h")
# SPIRV-Cross: compiled WITHOUT the util_likely.h inject — spirv_msl.cpp declares
# a variable literally named `force_inline`, which the macro would mangle.
SCFLAGS=(-std=c++17 "${COMMON[@]}")

objname() { echo "$OBJ/$(echo "${1#$ROOT/}" | tr '/.' '__').o"; }

# mode 0 = skip if the object already exists (stable vendor trees that never
#          change between builds); avoids even invoking the compiler.
# mode 1 = always invoke the compiler and let sccache decide (header-correct:
#          edits to any included header — including generated FF SPIR-V — bust
#          the cache; an unchanged TU is a fast sccache hit).
PIDS=(); FAILED=0; OBJS=()
compile_one() { # <src> <mode:0|1> <compiler-and-flags...>
  local src="$1" mode="$2"; shift 2
  local obj; obj="$(objname "$src")"
  [[ "$mode" == 0 && -f "$obj" ]] && return
  echo "[dxvk] CC $(basename "$src")"
  "$@" -c -o "$obj" "$src" || { echo "[dxvk] FAILED: $src"; rm -f "$obj"; return 1; }
}
queue() { # <bust> <compiler-and-flags...> ; reads source list from remaining args after --
  local bust="$1"; shift
  local -a flags=() srcs=(); local seen=0
  for a in "$@"; do
    if [[ "$a" == "--" ]]; then seen=1; continue; fi
    if (( seen )); then srcs+=("$a"); else flags+=("$a"); fi
  done
  for src in "${srcs[@]}"; do
    OBJS+=("$(objname "$src")")
    compile_one "$src" "$bust" "${flags[@]}" &
    PIDS+=($!)
    if (( ${#PIDS[@]} >= NJOBS )); then
      for p in "${PIDS[@]}"; do wait "$p" || FAILED=1; done; PIDS=()
    fi
  done
}

echo "[dxvk] compiling objects ($NJOBS jobs${SCCACHE:+, sccache})"
# v2 frontend/shim/util: always invoke (sccache is header-aware so a shim or
# generated-FF-header edit correctly busts; unchanged TUs are fast cache hits).
queue 1 $CXX "${V2FLAGS[@]}" -- \
  "$V2"/d3d9/*.cpp "$V2"/dxvk/*.cpp "$V2"/spirv/spirv_code_buffer.cpp \
  "${UTIL_SRCS[@]}"
# dxbc-spirv shader IR + SPIRV-Cross: stable vendor trees — skip if already built.
queue 0 $CXX "${V2FLAGS[@]}" -- "${SHADER_SRCS[@]}"
queue 0 $CXX "${SCFLAGS[@]}" -- "${SPIRV_CROSS_SRCS[@]}"
# sha1.c (C).
queue 1 $CC -std=c11 "${COMMON[@]}" -- "$V2/util/sha1/sha1.c"
for p in "${PIDS[@]}"; do wait "$p" || FAILED=1; done
(( FAILED )) && { echo "[dxvk] compile errors — aborting before link"; exit 1; }

echo "[dxvk] linking d3d9.dll (i686 PE)"
$MINGW-g++ -shared -o "$BUILD/d3d9.dll" \
  "${OBJS[@]}" \
  "$V2/d9mt/api/d3d9.def" \
  -Wl,--gc-sections \
  -L "$WINEMETAL" -lwinemetal \
  -L "$D9MTMETAL" -ld9mtmetal32 \
  -luuid -lgdi32 -luser32 -lole32 \
  `# gdi32: CreateBitmap/DeleteObject (HW cursor) + D3DKMT* (shared-resource validate)` \
  -static -static-libgcc -static-libstdc++ \
  -Wl,--enable-stdcall-fixup

echo "[dxvk] compiling asm_shader.exe (D3DXAssembleShader tool for test shaders)"
$MINGW-gcc -O2 -o "$BUILD/asm_shader.exe" "$V2/test/asm_shader.c" -ld3dx9_43

echo "[dxvk] compiling code.exe"
$MINGW-gcc -O2 -o "$BUILD/code.exe" "$V2/test/code.c" -ld3d9 -luser32 -lgdi32

echo "[dxvk] compiling code_shader.exe"
$MINGW-gcc -O2 -I"$V2/test" -o "$BUILD/code_shader.exe" "$V2/test/code_shader.c" \
  -ld3d9 -luser32 -lgdi32

echo "[dxvk] compiling code_index.exe"
$MINGW-gcc -O2 -I"$V2/test" -o "$BUILD/code_index.exe" "$V2/test/code_index.c" \
  -ld3d9 -luser32 -lgdi32

echo "[dxvk] compiling code_texture.exe"
$MINGW-gcc -O2 -I"$V2/test" -o "$BUILD/code_texture.exe" "$V2/test/code_texture.c" \
  -ld3d9 -luser32 -lgdi32

echo "[dxvk] compiling code_blend.exe"
$MINGW-gcc -O2 -I"$V2/test" -o "$BUILD/code_blend.exe" "$V2/test/code_blend.c" \
  -ld3d9 -luser32 -lgdi32

echo "[dxvk] compiling code_depth.exe"
$MINGW-gcc -O2 -I"$V2/test" -o "$BUILD/code_depth.exe" "$V2/test/code_depth.c" \
  -ld3d9 -luser32 -lgdi32

echo "[dxvk] compiling code_dxt.exe"
$MINGW-gcc -O2 -I"$V2/test" -o "$BUILD/code_dxt.exe" "$V2/test/code_dxt.c" \
  -ld3d9 -luser32 -lgdi32

echo "[dxvk] compiling code_mip.exe"
$MINGW-gcc -O2 -I"$V2/test" -o "$BUILD/code_mip.exe" "$V2/test/code_mip.c" -ld3d9 -luser32 -lgdi32

echo "[dxvk] compiling code_stencil.exe"
$MINGW-gcc -O2 -I"$V2/test" -o "$BUILD/code_stencil.exe" "$V2/test/code_stencil.c" -ld3d9 -luser32 -lgdi32

echo "[dxvk] compiling code_strip.exe"
$MINGW-gcc -O2 -I"$V2/test" -o "$BUILD/code_strip.exe" "$V2/test/code_strip.c" -ld3d9 -luser32 -lgdi32

echo "[dxvk] compiling code_cull.exe"
$MINGW-gcc -O2 -I"$V2/test" -o "$BUILD/code_cull.exe" "$V2/test/code_cull.c" -ld3d9 -luser32 -lgdi32

echo "[dxvk] compiling code_alpha.exe"
$MINGW-gcc -O2 -I"$V2/test" -o "$BUILD/code_alpha.exe" "$V2/test/code_alpha.c" -ld3d9 -luser32 -lgdi32

echo "[dxvk] compiling code_rtt.exe"
$MINGW-gcc -O2 -I"$V2/test" -o "$BUILD/code_rtt.exe" "$V2/test/code_rtt.c" \
  -ld3d9 -luser32 -lgdi32

echo "[dxvk] done:"; ls -la "$BUILD/d3d9.dll" "$BUILD"/code*.exe
