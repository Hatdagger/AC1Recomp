#!/usr/bin/env bash
# Build Armored Core: Master of Arena for Linux and wrap it as a self-contained
# AppImage that starts the game's own menu (no multi-game launcher).
# Usage: scripts/build_appimage_moa.sh [build-dir] [output.AppImage]
#   SKIP_BUILD=1  wrap an already built build-dir instead of building it.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="${1:-build/pkg-linux-moa}"
VERSION="$(tr -d '[:space:]' <"${ROOT}/VERSION")"
if [[ -n "${RELEASE_VERSION:-}" ]]; then VERSION="${RELEASE_VERSION}"; fi
NAME="armored-core-moa-${VERSION}-linux-x64"
OUTPUT="${2:-${ROOT}/dist/${NAME}.AppImage}"
JOBS="${BUILD_JOBS:-$(nproc)}"
EXE=ArmoredCoreMasterOfArena
cd "${ROOT}"

if [[ -z "${SKIP_BUILD:-}" ]]; then
  cmake -S . -B "${BUILD_DIR}" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DPSX_ENABLE_VULKAN=ON \
    -DPSXRECOMP_REQUIRE_GAME_C=ON
  cmake --build "${BUILD_DIR}" --target psx-master-of-arena -j"${JOBS}"
fi
[[ -f "${BUILD_DIR}/${EXE}" ]] || { echo "error: ${BUILD_DIR}/${EXE} not found" >&2; exit 1; }

# Runnable files only: no build state, no settings or saves from this machine,
# and nothing belonging to the other two games.
PAYLOAD="${ROOT}/dist/stage/${NAME}"
rm -rf "${PAYLOAD}"
mkdir -p "${PAYLOAD}"
rsync -a \
  --exclude='CMakeFiles/' --exclude='CMakeCache.txt' --exclude='build.ninja' \
  --exclude='*.cmake' --exclude='.ninja_*' --exclude='*_vkgen/' --prune-empty-dirs \
  --exclude='*.a' --exclude='*.c' --exclude='*.rc' --exclude='*.log' \
  --exclude='*_generated_sources.txt' \
  --exclude='psx_mod_catalog_*.txt' --exclude='psxrecomp_*.txt' \
  --exclude='Testing/' --exclude='CTestTestfile.cmake' --exclude='_deps/' --exclude='psx_execution/' \
  --exclude='launcher.ini' --exclude='launcher-window.ini' --exclude='config.ini' \
  --exclude='settings.toml' --exclude='keybinds.ini' --exclude='input.ini' \
  --exclude='disc.cfg' --exclude='bios.cfg' --exclude='rom.cfg' \
  --exclude='saves/' --exclude='psx_crash.txt' --exclude='psx_last_run_report.json' \
  --exclude='AC1' --exclude='AC1.*' --exclude='ArmoredCoreProjectPhantasma*' \
  --exclude='ArmoredCoreLauncher*' \
  --exclude='games/armored-core/' --exclude='games/project-phantasma/' \
  --exclude='/*.cue' --exclude='/*.bin' --exclude='/*.chd' --exclude='/*.iso' --exclude='/*.img' \
  --exclude='games/*/disc/' --exclude='/mods/new/' --exclude='*.mcd' --exclude='*.mcr' \
  "${BUILD_DIR}"/ "${PAYLOAD}"/
for file in "${EXE}" "${EXE}.execution.json" games/master-of-arena/game.toml \
            bios/openbios.bin bios/OpenBIOS.LICENSE; do
  [[ -s "${PAYLOAD}/${file}" ]] || { echo "error: ${file} is missing from the payload" >&2; exit 1; }
done
[[ -d "${PAYLOAD}/assets" ]] || { echo "error: assets/ is missing from the payload" >&2; exit 1; }
mkdir -p "${PAYLOAD}/games/master-of-arena/disc" "${PAYLOAD}/mods/new" "${PAYLOAD}/licenses"
printf 'Put your legally owned disc image here, or next to the AppImage file.\n\n%s\n' \
  'Armored Core: Master of Arena (USA, SLUS-01030 and SLUS-01081)' \
  >"${PAYLOAD}/games/master-of-arena/disc/README.txt"
cp psxrecomp/LICENSE "${PAYLOAD}/licenses/psxrecomp-LICENSE.txt"
cp psxrecomp/THIRD_PARTY_ATTRIBUTION.md "${PAYLOAD}/licenses/THIRD_PARTY_ATTRIBUTION.md"
cp recomp-ui/LICENSE "${PAYLOAD}/licenses/recomp-ui-LICENSE.txt"

# The compiler bundle: lets the game turn the code it loads from the disc into
# native code on the player's machine, in the background, as areas are visited.
# Without it that code stays in the interpreter (slow on a handheld). Needs the
# recompiler tools built (psxrecomp/tools/ci/build_emitters.sh); set
# MOA_RECOMP_DIR to their build folder.
if [[ -n "${MOA_RECOMP_DIR:-}" ]]; then
  python3 psxrecomp/tools/release_stage.py stage-toolchain \
    --stage "${PAYLOAD}" \
    --recomp-dir "${MOA_RECOMP_DIR}" \
    --recomp-tools psxrecomp/tools \
    --recomp-include psxrecomp/runtime/include \
    --dl-cache "${MOA_DL_CACHE:-${ROOT}/.cache/toolchain-dl}" \
    --platform linux
else
  echo "note: MOA_RECOMP_DIR not set; packaging without the compiler bundle" >&2
fi

TEMP="$(mktemp -d "${TMPDIR:-/tmp}/moa-appimage.XXXXXXXX")"
trap 'rm -rf "${TEMP}"' EXIT

# The game writes settings, memory cards and mods beside its executable, and
# an AppImage is read-only, so AppRun keeps a writable copy of the game under
# the user's data folder and carries the player's files forward to each new
# build.
BUILD_ID="$( { printf 'moa-appimage-layout-v1\n'; sha256sum "${PAYLOAD}/${EXE}"; } | sha256sum | cut -c1-16)"
cat >"${TEMP}/AppRun" <<EOF
#!/bin/sh
set -eu
APPDIR=\$(CDPATH= cd -- "\$(dirname -- "\$0")" && pwd)
PAYLOAD="\$APPDIR/usr/share/${NAME}"
DATA_ROOT="\${XDG_DATA_HOME:-\${HOME:?HOME must be set}/.local/share}/armored-core-moa"
RUNTIME="\$DATA_ROOT/runtime-${VERSION}-${BUILD_ID}"
APP_EXE="${EXE}"
# Memory cards live in one place for every build, outside the per-build copy.
CARDS="\$DATA_ROOT/Memory Card"
mkdir -p "\$DATA_ROOT" "\$CARDS"

if [ ! -d "\$RUNTIME" ]; then
    TEMP_RUNTIME=\$(mktemp -d "\$DATA_ROOT/.runtime.XXXXXXXX")
    cp -a "\$PAYLOAD/." "\$TEMP_RUNTIME/"
    cp -a "\$APPDIR/usr/bin/\$APP_EXE" "\$TEMP_RUNTIME/\$APP_EXE"
    cp -a "\$APPDIR/usr/bin/\$APP_EXE.execution.json" "\$TEMP_RUNTIME/\$APP_EXE.execution.json"
    PREVIOUS=\$(find "\$DATA_ROOT" -mindepth 1 -maxdepth 1 -type d -name 'runtime-*' ! -path "\$RUNTIME" -printf '%T@ %p\\n' | sort -nr | sed -n '1s/^[^ ]* //p')
    if [ -n "\$PREVIOUS" ]; then
        for file in game.toml game_options.toml settings.toml keybinds.ini input.ini; do
            if [ -f "\$PREVIOUS/games/master-of-arena/\$file" ]; then
                cp -a "\$PREVIOUS/games/master-of-arena/\$file" "\$TEMP_RUNTIME/games/master-of-arena/\$file"
            fi
        done
        for file in launcher.ini launcher-window.ini config.ini settings.toml keybinds.ini input.ini disc.cfg bios.cfg; do
            if [ -f "\$PREVIOUS/\$file" ]; then cp -a "\$PREVIOUS/\$file" "\$TEMP_RUNTIME/\$file"; fi
        done
        # Whole folders the player owns. Files the new build ships win; the
        # player's own (cards, installed mods, a disc image) are added to them.
        # Cards from a build that kept them in its own folder.
        if [ -d "\$PREVIOUS/saves" ]; then
            cp -an "\$PREVIOUS/saves/." "\$CARDS/" || true
        fi
        # Code already compiled from the disc, and the record of what was seen.
        if [ -f "\$PREVIOUS/overlay_captures.json" ]; then
            cp -a "\$PREVIOUS/overlay_captures.json" "\$TEMP_RUNTIME/overlay_captures.json"
        fi
        for dir in saves mods netplay cache games/master-of-arena/disc; do
            if [ -d "\$PREVIOUS/\$dir" ]; then
                mkdir -p "\$TEMP_RUNTIME/\$dir"
                cp -an "\$PREVIOUS/\$dir/." "\$TEMP_RUNTIME/\$dir/" || true
            fi
        done
    fi
    if ! mv -T "\$TEMP_RUNTIME" "\$RUNTIME" 2>/dev/null; then
        rm -rf "\$TEMP_RUNTIME"
    fi
fi

# A disc image may sit next to the AppImage file itself.
if [ -n "\${APPIMAGE:-}" ]; then
    PSX_DISC_SEARCH_DIR=\$(dirname -- "\$APPIMAGE")
    export PSX_DISC_SEARCH_DIR
    # The game's update page replaces this file; it cannot see APPIMAGE (unset
    # below), so the path travels under this name.
    MOA_APPIMAGE="\$APPIMAGE"
    export MOA_APPIMAGE
fi
export LD_LIBRARY_PATH="\$APPDIR/usr/lib\${LD_LIBRARY_PATH:+:\$LD_LIBRARY_PATH}"
unset APPIMAGE
# Keep the game's own text output from this run and the one before it, so a
# problem can be reported by sending a file instead of starting from a terminal.
LOG="\$DATA_ROOT/last_run.log"
if [ -f "\$LOG" ]; then mv -f "\$LOG" "\$DATA_ROOT/previous_run.log" 2>/dev/null || true; fi
exec "\$RUNTIME/\$APP_EXE" --memcard-dir "\$CARDS" "\$@" >"\$LOG" 2>&1
EOF
chmod 0755 "${TEMP}/AppRun"
cat >"${TEMP}/armored-core-moa.desktop" <<'EOF'
[Desktop Entry]
Type=Application
Name=Armored Core: Master of Arena
Exec=AppRun
Icon=armored-core-moa
Categories=Game;
Terminal=false
EOF

mkdir -p "$(dirname "${OUTPUT}")"
rm -f "${OUTPUT}"
"${ROOT}/psxrecomp/tools/package_appimage.sh" \
  --framework "${ROOT}/psxrecomp" \
  --payload "${PAYLOAD}" \
  --exe-name "${EXE}" \
  --payload-name "${NAME}" \
  --app-run "${TEMP}/AppRun" \
  --desktop-file "${TEMP}/armored-core-moa.desktop" \
  --icon "${ROOT}/assets/psxrecomp.png" \
  --output "${OUTPUT}" \
  --jobs "${JOBS}"

rm -rf "${PAYLOAD}"
echo "Built ${OUTPUT}"
