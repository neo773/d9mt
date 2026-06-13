#!/usr/bin/env bash
# Build the vendored DXVK v2.7.1 d3d9 front-end into build/d3d9fe.dll with
# every backend (DxvkContext/DxvkDevice/...) symbol resolved by abort() stubs
# in src/d3d9fe/stubs.cpp.  Completely separate from scripts/build.sh (the
# working hand-rolled d3d9.dll target) — never touches build/d3d9.dll.
#
# Objects are cached in build/dxvkfe-obj; only out-of-date TUs recompile, so
# the stub-iteration loop (edit stubs.cpp -> rerun) is fast.
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
V="$ROOT/vendor/dxvk"
BUILD="$ROOT/build"
GEN="$BUILD/dxvkfe-gen"

# RELEASE=1 -> max-speed production build: -O3 + LTO, asserts stripped, and
# the in-engine trace + file logging COMPILED OUT (not just runtime-disabled),
# so there is zero instrumentation on the hot path. A separate object cache
# keeps the dev (-O2, traced) and release object trees from clashing — flags
# only take effect on a recompile, and mtime caching can't see a flag change.
RELEASE="${RELEASE:-0}"
if [[ "$RELEASE" == 1 ]]; then
  OBJ="$BUILD/dxvkfe-obj-release"
  # -flto is omitted: mingw's DLL auto-export chokes on LTO'd std::regex
  # template symbols ("wrong type"). -O3 + NDEBUG + compiled-out trace/log is
  # the bulk of the win without that fight.
  OPT_FLAGS=(-O3 -funroll-loops -fno-math-errno
             -DNDEBUG -DD9MT_NO_TRACE -DD9MT_NO_LOG)
  LINK_OPT=(-O3)
  echo "[dxvkfe] RELEASE build — max speed (-O3, no trace, no logging)"
else
  OBJ="$BUILD/dxvkfe-obj"
  OPT_FLAGS=(-O2)
  LINK_OPT=()
fi
mkdir -p "$OBJ" "$GEN"

CXX=i686-w64-mingw32-g++
CC=i686-w64-mingw32-gcc

# Flags per docs/BACKEND-SURFACE.md §6.4 (mirrors upstream meson.build)
COMMON_FLAGS=(
  "${OPT_FLAGS[@]}" -w
  -msse -msse2 -msse3 -mfpmath=sse -mpreferred-stack-boundary=2
  -ffunction-sections -fdata-sections
  -DNOMINMAX -D_WIN32_WINNT=0xa00 -DDXVK_WSI_WIN32
  -I "$GEN"
  -I "$V/src"
  -I "$V/src/dxvk"
  -I "$V/include/vulkan/include"
  -I "$V/include/spirv/include"
)
CXXFLAGS=(-std=c++17 "${COMMON_FLAGS[@]}")
CFLAGS=("${COMMON_FLAGS[@]}")

# ---------------------------------------------------------------------------
# Generated headers: 7 glslang SPIR-V arrays for d3d9_format_helpers.cpp
# (upstream: glslang --quiet --target-env vulkan1.3 --vn <basename>)
# ---------------------------------------------------------------------------
GLSLANG="$(command -v glslang || command -v glslangValidator)"
for comp in "$V"/src/d3d9/shaders/d3d9_convert_*.comp; do
  base="$(basename "$comp" .comp)"
  out="$GEN/$base.h"
  if [[ ! -f "$out" || "$comp" -nt "$out" ]]; then
    echo "[dxvkfe] glslang $base"
    "$GLSLANG" --quiet --target-env vulkan1.3 --vn "$base" -o "$out" "$comp" || exit 1
  fi
done

# ---------------------------------------------------------------------------
# Translation units
# ---------------------------------------------------------------------------
CPP_SOURCES=()

# d3d9 front-end: every .cpp in the vendored dir
for f in "$V"/src/d3d9/*.cpp; do CPP_SOURCES+=("$f"); done

# dxso: all 13 .cpp (incl. dxso_options.cpp — front-end build owns the device)
for f in "$V"/src/dxso/*.cpp; do CPP_SOURCES+=("$f"); done

# spirv
CPP_SOURCES+=(
  "$V/src/spirv/spirv_code_buffer.cpp"
  "$V/src/spirv/spirv_compression.cpp"
  "$V/src/spirv/spirv_module.cpp"
)

# dxvk TUs that are pure CPU-side helpers, vendored verbatim from v2.7.1
# (format table, util packing, CS thread machinery, state normalization)
CPP_SOURCES+=(
  "$V/src/dxvk/dxvk_shader.cpp"
  "$V/src/dxvk/dxvk_pipelayout.cpp"
  "$V/src/dxvk/dxvk_shader_key.cpp"
  "$V/src/dxvk/dxvk_format.cpp"
  "$V/src/dxvk/dxvk_util.cpp"
  "$V/src/dxvk/dxvk_cs.cpp"
  "$V/src/dxvk/dxvk_constant_state.cpp"
)

# util (per BACKEND-SURFACE.md §6.2; sha1.c is plain C, handled below)
CPP_SOURCES+=(
  "$V/src/util/thread.cpp"
  "$V/src/util/util_env.cpp"
  "$V/src/util/util_string.cpp"
  "$V/src/util/util_flush.cpp"
  "$V/src/util/util_gdi.cpp"
  "$V/src/util/util_luid.cpp"
  "$V/src/util/util_matrix.cpp"
  "$V/src/util/util_shared_res.cpp"
  "$V/src/util/util_fps_limiter.cpp"
  "$V/src/util/util_sleep.cpp"
  "$V/src/util/log/log.cpp"
  "$V/src/util/log/log_debug.cpp"
  "$V/src/util/config/config.cpp"
  "$V/src/util/sha1/sha1_util.cpp"
  "$V/src/util/sync/sync_recursive.cpp"
  "$V/src/util/com/com_guid.cpp"
  "$V/src/util/com/com_private_data.cpp"
)

# wsi win32 (wsi_edid.cpp omitted: hard libdisplay-info dep; stubs.cpp
# provides parseColorimetryInfo -> std::nullopt)
CPP_SOURCES+=(
  "$V/src/wsi/wsi_platform.cpp"
  "$V/src/wsi/win32/wsi_platform_win32.cpp"
  "$V/src/wsi/win32/wsi_monitor_win32.cpp"
  "$V/src/wsi/win32/wsi_window_win32.cpp"
)

# vulkan: enum name tables only (no libvulkan dependency); the loader itself
# is stubbed in stubs.cpp
CPP_SOURCES+=(
  "$V/src/vulkan/vulkan_names.cpp"
)

# SPIRV-Cross (verbatim Khronos): SPIR-V -> MSL for the Draw stage
# (d9mt_shader.cpp), same TU set as scripts/build.sh
SC="$ROOT/vendor/spirv-cross"
CPP_SOURCES+=(
  "$SC/spirv_cross.cpp"
  "$SC/spirv_parser.cpp"
  "$SC/spirv_cross_parsed_ir.cpp"
  "$SC/spirv_cfg.cpp"
  "$SC/spirv_glsl.cpp"
  "$SC/spirv_msl.cpp"
)

# our Metal backend + remaining stub layer (everything in src/d3d9fe)
for f in "$ROOT"/src/d3d9fe/*.cpp; do CPP_SOURCES+=("$f"); done

C_SOURCES=(
  "$V/src/util/sha1/sha1.c"
)

# ---------------------------------------------------------------------------
# Compile (cached, parallel)
# ---------------------------------------------------------------------------
objname() { # unique object name from path relative to repo root
  local rel="${1#$ROOT/}"
  echo "$OBJ/$(echo "$rel" | tr '/.' '__').o"
}

PIDS=()
FAILED=0
NJOBS=8
BACKEND_HDR="$ROOT/src/d3d9fe/d9mt_backend.h"
DRAW_HDR="$ROOT/src/d3d9fe/d9mt_draw.h"
compile_one() { # <src> <compiler...>
  local src="$1"; shift
  local obj; obj="$(objname "$src")"
  # backend TUs additionally depend on the shared backend headers
  if [[ "$src" == "$ROOT/src/d3d9fe/"* && -f "$obj" \
     && ( "$BACKEND_HDR" -nt "$obj" || "$DRAW_HDR" -nt "$obj" ) ]]; then
    rm -f "$obj"
  fi
  if [[ ! -f "$obj" || "$src" -nt "$obj" ]]; then
    echo "[dxvkfe] CC $(basename "$src")"
    "$@" -c -o "$obj" "$src" || { echo "[dxvkfe] FAILED: $src"; rm -f "$obj"; return 1; }
  fi
}

OBJS=()
for src in "${CPP_SOURCES[@]}"; do
  OBJS+=("$(objname "$src")")
  compile_one "$src" "$CXX" "${CXXFLAGS[@]}" &
  PIDS+=($!)
  if (( ${#PIDS[@]} >= NJOBS )); then
    for p in "${PIDS[@]}"; do wait "$p" || FAILED=1; done
    PIDS=()
  fi
done
for src in "${C_SOURCES[@]}"; do
  OBJS+=("$(objname "$src")")
  compile_one "$src" "$CC" "${CFLAGS[@]}" &
  PIDS+=($!)
done
for p in "${PIDS[@]}"; do wait "$p" || FAILED=1; done
if (( FAILED )); then
  echo "[dxvkfe] compile errors — aborting before link"
  exit 1
fi

# ---------------------------------------------------------------------------
# Link
# ---------------------------------------------------------------------------
echo "[dxvkfe] linking d3d9fe.dll"
"$CXX" -shared -o "$BUILD/d3d9fe.dll" \
  ${LINK_OPT[@]+"${LINK_OPT[@]}"} \
  "${OBJS[@]}" \
  -Wl,--gc-sections \
  -static -static-libgcc -static-libstdc++ \
  -Wl,--file-alignment=4096,--enable-stdcall-fixup,--kill-at \
  -L "$ROOT/prebuilt" -lwinemetal \
  -L "$BUILD/d9mtmetal" -ld9mtmetal32 \
  -luser32 -lgdi32 -lsetupapi -luuid \
  2> "$BUILD/dxvkfe-link.log"
LINK_RC=$?
if (( LINK_RC != 0 )); then
  UND=$(grep -c "undefined reference" "$BUILD/dxvkfe-link.log" || true)
  echo "[dxvkfe] LINK FAILED ($UND undefined-reference lines) — see $BUILD/dxvkfe-link.log"
  exit 1
fi
cat "$BUILD/dxvkfe-link.log" >&2
echo "[dxvkfe] done:"
ls -la "$BUILD/d3d9fe.dll"
