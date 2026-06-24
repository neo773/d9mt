#!/usr/bin/env bash
# Assemble D3D9 shader assembly (test/shaders/*.asm) into byte-array .inc files
# the test executables #include. Uses asm_shader.exe (D3DXAssembleShader) under
# Wine in the bottle, since D3DXAssembleShader needs the bottle's d3dx9. Run
# this after editing a .asm; commit the regenerated .inc.
set -euo pipefail
V2="$(cd "$(dirname "$0")/.." && pwd)"
CX="/Applications/CrossOver.app/Contents/SharedSupport/CrossOver"
WINE="$CX/bin/wine"
BOTTLE="Rockstar Games Launcher"
TESTDIR="$HOME/Library/Application Support/CrossOver/Bottles/$BOTTLE/drive_c/d9mt-test"

mkdir -p "$TESTDIR"
cp "$V2/build/asm_shader.exe" "$TESTDIR/"

for asm in "$V2"/test/shaders/*.asm; do
  name="$(basename "$asm" .asm)"
  cp "$asm" "$TESTDIR/$name.asm"
  ( cd "$TESTDIR" && "$WINE" --bottle "$BOTTLE" "$TESTDIR/asm_shader.exe" "$name.asm" "$name.bin" )
  python3 -c "import sys; print(','.join(str(b) for b in open(sys.argv[1],'rb').read()))" \
    "$TESTDIR/$name.bin" > "$V2/test/${name}_bytecode.inc"
  echo "[asm] $name.asm -> ${name}_bytecode.inc"
done
