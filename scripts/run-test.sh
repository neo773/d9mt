#!/usr/bin/env bash
# Install DXMT's prebuilt winemetal bridge into CrossOver's wine tree,
# register it as a builtin in the bottle, copy d9mt artifacts into the
# bottle, and launch triangle.exe.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$ROOT/build"
PREBUILT="$ROOT/prebuilt"

CX="/Applications/CrossOver.app/Contents/SharedSupport/CrossOver"
WINE="$CX/bin/wine"
WINE_LIB="$CX/lib/wine"
BOTTLE="Rockstar Games Launcher"
BOTTLE_DIR="$HOME/Library/Application Support/CrossOver/Bottles/$BOTTLE"
TESTDIR="$BOTTLE_DIR/drive_c/d9mt-test"

[ -f "$BUILD/d3d9.dll" ] || { echo "ERROR: run scripts/build.sh first"; exit 1; }
[ -f "$PREBUILT/winemetal32.dll" ] || { echo "ERROR: run scripts/fetch-winemetal.sh first"; exit 1; }

install_file() {
  local src="$1" dst="$2"
  if [ -f "$dst" ] && [ ! -f "$dst.d9mt-bak" ]; then
    cp -p "$dst" "$dst.d9mt-bak"
    echo "[install] backed up $dst -> $dst.d9mt-bak"
  fi
  cp "$src" "$dst"
  echo "[install] $src -> $dst"
}

echo "[install] winemetal into CrossOver wine tree"
install_file "$PREBUILT/winemetal32.dll" "$WINE_LIB/i386-windows/winemetal.dll"
install_file "$PREBUILT/winemetal.dll"   "$WINE_LIB/x86_64-windows/winemetal.dll"
install_file "$PREBUILT/winemetal.so"    "$WINE_LIB/x86_64-unix/winemetal.so"

echo "[install] registering winemetal as builtin in bottle '$BOTTLE'"
"$WINE" --bottle "$BOTTLE" reg add 'HKCU\Software\Wine\DllOverrides' \
  /v winemetal /d builtin /f >/dev/null 2>&1 || true

echo "[install] copying d9mt test files to $TESTDIR"
mkdir -p "$TESTDIR"
cp "$BUILD/d3d9.dll" "$BUILD/triangle.exe" "$TESTDIR/"

echo "[run] launching triangle.exe (log: $TESTDIR/d9mt.log)"
cd "$TESTDIR"
WINEDLLOVERRIDES="d3d9=n" "$WINE" --bottle "$BOTTLE" \
  start /unix "$TESTDIR/triangle.exe"

echo "[run] launched. Expect a window with a red/green/blue triangle on"
echo "      dark blue. Check $TESTDIR/d9mt.log for driver-side status."
