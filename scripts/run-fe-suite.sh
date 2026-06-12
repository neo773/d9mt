#!/bin/bash
# Run d3d9fe test suite in the isolated d9mtfe-test dir (never touches d9mt-test / GTA dirs).
set -u
WINE="/Applications/CrossOver.app/Contents/SharedSupport/CrossOver/bin/wine"
BOTTLE="Rockstar Games Launcher"
TESTDIR="$HOME/Library/Application Support/CrossOver/Bottles/$BOTTLE/drive_c/d9mtfe-test"
cd "$TESTDIR" || exit 1

run_one() {
  local exe="$1" out="$2" timeout="${3:-35}"
  rm -f "$TESTDIR/$out" "$TESTDIR/d3d9fe.log" "$TESTDIR/d3d9fe-stub.log"
  echo "==== RUN $exe ===="
  WINEDLLOVERRIDES="d3d9=n" DXVK_LOG_LEVEL=info "$WINE" --bottle "$BOTTLE" \
    "C:\\d9mtfe-test\\$exe" > "$TESTDIR/wine-suite-$exe.txt" 2>&1 &
  local i=0
  while [ $i -lt "$timeout" ]; do
    sleep 1; i=$((i+1))
    if [ -f "$TESTDIR/$out" ] && grep -qE "PASS|FAIL" "$TESTDIR/$out"; then
      break
    fi
  done
  sleep 2
  "$WINE" --bottle "$BOTTLE" taskkill /im "$exe" /f >/dev/null 2>&1
  sleep 2
  echo "---- $out ----"
  cat "$TESTDIR/$out" 2>/dev/null || echo "(no $out)"
  echo "---- stub log ----"
  cat "$TESTDIR/d3d9fe-stub.log" 2>/dev/null || echo "(none)"
  echo "---- err/warn in d3d9fe.log ----"
  grep -E "err:|warn:" "$TESTDIR/d3d9fe.log" 2>/dev/null | head -15 || echo "(none)"
  cp "$TESTDIR/d3d9fe.log" "$TESTDIR/d3d9fe-$exe.log" 2>/dev/null
  echo
}

for spec in "$@"; do
  exe="${spec%%:*}"; out="${spec##*:}"
  run_one "$exe" "$out"
done
echo "==== SUITE DONE ===="
