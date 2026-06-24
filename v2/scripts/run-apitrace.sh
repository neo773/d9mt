#!/usr/bin/env bash
# Capture an apitrace .trace of a v2 test exe's D3D9 calls. The upstream apitrace
# d3d9 proxy wrapper sits in the app dir, traces every call, and forwards to the
# real d3d9 (our Metal driver, installed into system32). Produces a .trace file
# replayable/inspectable with apitrace (qapitrace / d3dretrace).
set -euo pipefail
V2="$(cd "$(dirname "$0")/.." && pwd)"
CX="/Applications/CrossOver.app/Contents/SharedSupport/CrossOver"
WINE="$CX/bin/wine"
BOTTLE="Rockstar Games Launcher"
BD="$HOME/Library/Application Support/CrossOver/Bottles/$BOTTLE"
SYS32="$BD/drive_c/windows/system32"
TESTDIR="$BD/drive_c/d9mt-test"
APITRACE_D3D9="${APITRACE_D3D9:-/tmp/apitrace-w32/apitrace-14.0-win32/lib/wrappers/d3d9.dll}"
EXE="${1:-code_alpha.exe}"

pkill -9 -f code_ 2>/dev/null || true
sleep 1

# apitrace proxy is the app-dir d3d9 the exe loads first; it forwards to our
# Metal driver via APITRACE_FORCE_MODULE_PATH (distinct name avoids the circular
# self-load and the system32 builtin-resolution problems).
cp "$APITRACE_D3D9" "$TESTDIR/d3d9.dll"
cp "$V2/build/d3d9.dll" "$TESTDIR/d3d9_metal.dll"
cp "$V2/build/$EXE" "$TESTDIR/"
rm -f "$TESTDIR"/*.trace
rm -f "$SYS32/d3d9.dll"

cd "$TESTDIR"
export TRACE_FILE='C:\d9mt-test\app.trace'
export APITRACE_FORCE_MODULE_PATH='C:\d9mt-test\d3d9_metal.dll'
export WINEDLLOVERRIDES='d3d9=n;winemetal=n'
"$WINE" --bottle "$BOTTLE" start /unix "$TESTDIR/$EXE" >/dev/null 2>&1 &
echo "[apitrace] launched $EXE (pid $!) — forwarding to d3d9_metal.dll"
