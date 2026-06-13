#!/usr/bin/env bash
# fetch-winemetal.sh
# Downloads the latest DXMT release from github.com/3Shain/dxmt and extracts
# ONLY the winemetal binaries needed by d9mt into d9mt/prebuilt/.
#
# Artifact layout inside the release tarball (observed from v0.80):
#   <tag>/x86_64-windows/winemetal.dll   <- 64-bit PE builtin DLL
#   <tag>/i386-windows/winemetal.dll     <- 32-bit PE builtin DLL (WoW64)
#   <tag>/x86_64-unix/winemetal.so       <- 64-bit Mach-O unixlib
#
# CrossOver bottle install destinations (per DXMT install convention,
# which mirrors a Wine prefix layout):
#   winemetal.dll (64-bit)  -> <bottle>/drive_c/windows/system32/winemetal.dll
#   winemetal.dll (32-bit)  -> <bottle>/drive_c/windows/syswow64/winemetal.dll
#   winemetal.so            -> <bottle>/drive_c/windows/x86_64-unix/winemetal.so
#
# winemetal.dll MUST be marked as a Wine built-in DLL so that ntdll can
# resolve its unixlib handle at load time (see notes in README.md).
# DXMT's own installer achieves this by running `winebuild --builtin` on the
# DLL during the build (see src/winemetal/meson.build) which embeds the
# "Wine builtin DLL" marker in the PE header.  The prebuilt binaries from
# the release tarball already have this marker applied.
# To register as builtin with CrossOver, add to the bottle registry:
#   HKCU\Software\Wine\DllOverrides  "winemetal" = "builtin"
# or use winecfg / winetricks inside the bottle.

set -euo pipefail

REPO="3Shain/dxmt"
mkdir -p "$(dirname "$0")/../prebuilt"
PREBUILT_DIR="$(cd "$(dirname "$0")/../prebuilt" && pwd)"
TMPDIR_LOCAL="$(mktemp -d)"
trap 'rm -rf "$TMPDIR_LOCAL"' EXIT

echo "[fetch-winemetal] Querying latest DXMT release from $REPO ..."
LATEST_TAG=$(curl -fsSL \
    "https://api.github.com/repos/${REPO}/releases/latest" \
    | python3 -c "import sys,json; print(json.load(sys.stdin)['tag_name'])")

if [ -z "$LATEST_TAG" ]; then
    # Fall back to gh CLI if curl+python fails (e.g. rate-limited)
    LATEST_TAG=$(gh release list --repo "$REPO" --limit 1 | awk '{print $3}')
fi

echo "[fetch-winemetal] Latest release: $LATEST_TAG"

# Determine tarball name: DXMT releases a single combined tarball
TARBALL_NAME="dxmt-${LATEST_TAG}-builtin.tar.gz"
DOWNLOAD_URL="https://github.com/${REPO}/releases/download/${LATEST_TAG}/${TARBALL_NAME}"

echo "[fetch-winemetal] Downloading $DOWNLOAD_URL ..."
curl -fsSL -o "$TMPDIR_LOCAL/$TARBALL_NAME" "$DOWNLOAD_URL"

echo "[fetch-winemetal] Extracting winemetal files ..."
# Tag directory inside the tarball is the version string (e.g. "v0.80")
TAG_DIR="${LATEST_TAG}"

tar -xzf "$TMPDIR_LOCAL/$TARBALL_NAME" -C "$TMPDIR_LOCAL" \
    "${TAG_DIR}/x86_64-windows/winemetal.dll" \
    "${TAG_DIR}/i386-windows/winemetal.dll" \
    "${TAG_DIR}/x86_64-unix/winemetal.so" 2>/dev/null || \
tar -xzf "$TMPDIR_LOCAL/$TARBALL_NAME" -C "$TMPDIR_LOCAL"  # fallback: extract all then filter

mkdir -p "$PREBUILT_DIR"

cp "$TMPDIR_LOCAL/${TAG_DIR}/x86_64-windows/winemetal.dll" \
   "$PREBUILT_DIR/winemetal.dll"
cp "$TMPDIR_LOCAL/${TAG_DIR}/i386-windows/winemetal.dll" \
   "$PREBUILT_DIR/winemetal32.dll"
cp "$TMPDIR_LOCAL/${TAG_DIR}/x86_64-unix/winemetal.so" \
   "$PREBUILT_DIR/winemetal.so"

echo "[fetch-winemetal] Generating 32-bit import library (python objdump + dlltool)"
cd "$PREBUILT_DIR"
python3 - winemetal32.dll winemetal32.def <<'EOF'
import sys, subprocess, re
dll = sys.argv[1]
def_file = sys.argv[2]
out = subprocess.check_output(["i686-w64-mingw32-objdump", "-p", dll]).decode("utf-8")
start_idx = out.find("[Ordinal/Name Pointer] Table -- Ordinal Base 1")
if start_idx == -1:
    print("Error: Name Pointer Table not found in objdump output")
    sys.exit(1)
names = []
lines = out[start_idx:].splitlines()
for line in lines[1:]:
    if "Base Relocations" in line or "Relocations" in line:
        break
    m = re.search(r'\s+0\w+\s+(\w+)\s*$', line)
    if m:
        names.append(m.group(1))
with open(def_file, 'w') as f:
    f.write("LIBRARY winemetal.dll\nEXPORTS\n")
    for name in names:
        f.write(f"  {name}\n")
EOF
i686-w64-mingw32-dlltool -d winemetal32.def -l libwinemetal.a \
  --dllname winemetal.dll

echo "[fetch-winemetal] Done. Files written to $PREBUILT_DIR:"
ls -lh "$PREBUILT_DIR"

cat <<'EOF'

=== Install destinations (CrossOver wine tree, handled by run-test.sh) ===
  winemetal32.dll -> <CX>/lib/wine/i386-windows/winemetal.dll
  winemetal.dll   -> <CX>/lib/wine/x86_64-windows/winemetal.dll
  winemetal.so    -> <CX>/lib/wine/x86_64-unix/winemetal.so
  plus bottle registry: HKCU\Software\Wine\DllOverrides "winemetal"="builtin"
  where <CX> = /Applications/CrossOver.app/Contents/SharedSupport/CrossOver
EOF
