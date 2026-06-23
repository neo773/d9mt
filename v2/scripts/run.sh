#!/usr/bin/env bash
# Deploy the v2 d3d9.dll + code.exe into the Wine bottle and launch code.exe.
set -euo pipefail
V2="$(cd "$(dirname "$0")/.." && pwd)"
ROOT="$(cd "$V2/.." && pwd)"
CX="/Applications/CrossOver.app/Contents/SharedSupport/CrossOver"
WINE="$CX/bin/wine"
WINE_LIB="$CX/lib/wine"
BOTTLE="Rockstar Games Launcher"
BOTTLE_DIR="$HOME/Library/Application Support/CrossOver/Bottles/$BOTTLE"
TESTDIR="$BOTTLE_DIR/drive_c/d9mt-test"

# winemetal builtin bridge into the wine tree (as-is from DXMT, vendored)
cp "$ROOT/prebuilt/winemetal32.dll" "$WINE_LIB/i386-windows/winemetal.dll"
cp "$ROOT/prebuilt/winemetal.dll"   "$WINE_LIB/x86_64-windows/winemetal.dll"
cp "$ROOT/prebuilt/winemetal.so"    "$WINE_LIB/x86_64-unix/winemetal.so"
"$WINE" --bottle "$BOTTLE" reg add 'HKCU\Software\Wine\DllOverrides' \
  /v winemetal /d builtin /f >/dev/null 2>&1 || true

mkdir -p "$TESTDIR"
rm -f "$TESTDIR/v2.log"
cp "$V2/build/d3d9.dll" "$V2/build/code.exe" "$TESTDIR/"
echo "[v2] launching code.exe under wine"
cd "$TESTDIR"
WINEDLLOVERRIDES="d3d9=n" "$WINE" --bottle "$BOTTLE" \
  start /unix "$TESTDIR/code.exe" >/dev/null 2>&1 &
echo "[v2] launched pid $!"
