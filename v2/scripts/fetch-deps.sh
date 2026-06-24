#!/usr/bin/env bash
# Fetch + vendor the v2 frontend dependencies from upstream DXVK.
#
# v2 keeps DXVK's D3D9 frontend (v2/d3d9) and its kept C++ deps (util/spirv/wsi)
# plus the headers needed for types (vulkan/dxvk) and the new shader IR
# (dxbc-spirv + sm3). The dxvk/ + vulkan/ runtime is what v2 REPLACES with
# Metal; we keep their headers only for the types the frontend's seam uses.
#
# These trees are large and external, so they are gitignored and reproduced
# here rather than committed (see v2/.gitignore).
set -euo pipefail
V2="$(cd "$(dirname "$0")/.." && pwd)"
SRC="${1:-/tmp/dxvk-upstream}"

if [ ! -d "$SRC/.git" ]; then
  echo "[deps] cloning dxvk -> $SRC"
  git clone --depth 1 https://github.com/doitsujin/dxvk/ "$SRC"
fi
echo "[deps] fetching submodules (dxbc-spirv, vulkan/spirv headers)"
( cd "$SRC" && git submodule update --init --depth 1 \
    subprojects/dxbc-spirv include/vulkan include/spirv )

echo "[deps] vendoring kept C++ deps + dxvk/vulkan headers"
for d in util spirv wsi vulkan; do
  rm -rf "$V2/$d"; cp -R "$SRC/src/$d" "$V2/$d"
done
rm -rf "$V2/dxvk-ref"; cp -R "$SRC/src/dxvk" "$V2/dxvk-ref"  # reference only; v2/dxvk is our shim

echo "[deps] vendoring dxbc-spirv (sm3 + IR) + Khronos headers"
rm -rf "$V2/third_party/dxbc-spirv"
cp -R "$SRC/subprojects/dxbc-spirv" "$V2/third_party/dxbc-spirv"
rm -rf "$V2/third_party/dxbc-spirv/.git"
mkdir -p "$V2/third_party/khronos"
cp -R "$SRC/include/vulkan" "$V2/third_party/khronos/vulkan"
cp -R "$SRC/include/spirv"  "$V2/third_party/khronos/spirv"

echo "[deps] done. Frontend: v2/d3d9 (full upstream)."
