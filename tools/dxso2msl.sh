#!/usr/bin/env bash
# Full d9mt shader pipeline: D3D9 SM1-3 bytecode -> SPIR-V -> MSL -> metallib
# usage: dxso2msl.sh <in.vso|in.pso> <out-basename>
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
IN="$1"
BASE="$2"

[ -x "$ROOT/build/dxso2spv" ] || { echo "ERROR: run tools/build-host.sh first"; exit 1; }

"$ROOT/build/dxso2spv" "$IN" "$BASE.spv"

# DXVK 2.7 emits a runtime-sized sampler heap -> needs Metal argument
# buffers tier 2 (supported on Apple Silicon)
spirv-cross --msl --msl-version 30000 \
  --msl-argument-buffers --msl-argument-buffer-tier 2 \
  "$BASE.spv" --output "$BASE.metal"

xcrun -sdk macosx metal -o "$BASE.metallib" "$BASE.metal"

echo "$IN -> $BASE.spv -> $BASE.metal -> $BASE.metallib"
