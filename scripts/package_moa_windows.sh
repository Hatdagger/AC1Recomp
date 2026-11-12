#!/usr/bin/env bash
# Lay a Windows build of Master of Arena out as the player-facing folder:
#
#   <out>/Armored Core Moa.exe   the program to start
#   <out>/Memory Card/           memory cards
#   <out>/Mods/                  bundled and installed mods; Mods/new is the drop folder
#   <out>/RecompData/            the game and everything it needs
#   <out>/(disc images)          the player's own, never copied by this script
#
# Usage: scripts/package_moa_windows.sh <build-dir> [out-dir]
#   out-dir defaults to "Armored Core Moa" in the project root.
#
# Running it again over an existing folder updates the game and leaves the
# player's files alone: settings, memory cards and anything already in Mods/
# that the build does not ship are kept. The first run carries over settings,
# cards and mods found in the build folder, so a copy that was being played
# from the build folder moves across intact.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${1:?usage: $0 <build-dir> [out-dir]}"
OUT="${2:-${ROOT}/Armored Core Moa}"
SHELL_EXE="Armored Core Moa.exe"
GAME_EXE="ArmoredCoreMasterOfArena.exe"
BUILD="$(cd "${BUILD}" && pwd)"

for file in "${GAME_EXE}" "${SHELL_EXE}" games/master-of-arena/game.toml; do
  [[ -f "${BUILD}/${file}" ]] || { echo "error: ${BUILD}/${file} not found (build psx-master-of-arena and psx-moa-shell first)" >&2; exit 1; }
done
[[ -d "${BUILD}/assets" ]] || { echo "error: ${BUILD}/assets not found" >&2; exit 1; }

DATA="${OUT}/RecompData"
mkdir -p "${DATA}" "${OUT}/Memory Card" "${OUT}/Mods/new"
OUT="$(cd "${OUT}" && pwd)"
DATA="${OUT}/RecompData"

# Files that belong to the player or to this machine. Never overwritten.
USER_FILES=(launcher.ini launcher-window.ini config.ini settings.toml keybinds.ini
            input.ini disc.cfg bios.cfg rom.cfg
            games/master-of-arena/settings.toml games/master-of-arena/game_options.toml
            games/master-of-arena/keybinds.ini games/master-of-arena/input.ini)

# The game: runnable files only. No build state, none of the other games,
# no saves or mods (they have their own folders), no disc images.
EXCLUDES=(
  --exclude=./CMakeFiles --exclude=./CMakeCache.txt --exclude=./build.ninja
  --exclude=./cmake_install.cmake --exclude='./.ninja_*' --exclude='*_vkgen'
  --exclude='*.a' --exclude='*.c' --exclude='*.rc' --exclude='*.res' --exclude='*.obj' --exclude='*.o'
  --exclude='*_generated_sources.txt' --exclude='psx_mod_catalog_*.txt' --exclude='psxrecomp_*.txt'
  --exclude=./Testing --exclude=./CTestTestfile.cmake --exclude=./_deps --exclude=./psx_execution
  --exclude='*.log' --exclude=./psx_crash.txt --exclude=./psx_last_run_report.json
  --exclude=./saves --exclude=./mods
  --exclude="./${SHELL_EXE}"
  --exclude='./AC1.exe' --exclude='./AC1.*' --exclude='./ArmoredCoreProjectPhantasma*'
  --exclude='./ArmoredCoreLauncher*'
  --exclude=./games/armored-core --exclude=./games/project-phantasma
  --no-wildcards-match-slash
  --exclude='./games/*/disc'
  --exclude='./*.cue' --exclude='./*.bin' --exclude='./*.chd' --exclude='./*.iso' --exclude='./*.img'
  --wildcards-match-slash
  --exclude='*.mcd' --exclude='*.mcr'
)
for file in "${USER_FILES[@]}"; do EXCLUDES+=(--exclude="./${file}"); done
(cd "${BUILD}" && tar -cf - "${EXCLUDES[@]}" .) | (cd "${DATA}" && tar -xf -)
cp -f "${BUILD}/${SHELL_EXE}" "${OUT}/${SHELL_EXE}"

# Compiler runtime DLLs the exe asks for, when it was not linked statically:
# copied beside it so the folder runs on a PC without the build tools.
if command -v objdump >/dev/null 2>&1; then
  TOOL_DIRS=()
  for tool in gcc clang; do
    if command -v "${tool}" >/dev/null 2>&1; then TOOL_DIRS+=("$(dirname "$(command -v "${tool}")")"); fi
  done
  TOOL_DIRS+=("${BUILD}")
  pending=("${DATA}/${GAME_EXE}")
  while [[ ${#pending[@]} -gt 0 ]]; do
    current="${pending[0]}"; pending=("${pending[@]:1}")
    while read -r dll; do
      [[ -n "${dll}" && ! -f "${DATA}/${dll}" ]] || continue
      for dir in "${TOOL_DIRS[@]}"; do
        if [[ -f "${dir}/${dll}" ]]; then
          cp "${dir}/${dll}" "${DATA}/${dll}"
          pending+=("${DATA}/${dll}")
          echo "bundled ${dll}"
          break
        fi
      done
    done < <(objdump -p "${current}" 2>/dev/null | sed -n 's/^[[:space:]]*DLL Name: //p' | tr -d '\r')
  done
fi

# The player's files from the build folder, where the destination has none yet.
for file in "${USER_FILES[@]}"; do
  if [[ -f "${BUILD}/${file}" && ! -e "${DATA}/${file}" ]]; then
    mkdir -p "$(dirname "${DATA}/${file}")"
    cp -p "${BUILD}/${file}" "${DATA}/${file}"
  fi
done
if [[ -d "${BUILD}/saves" ]]; then
  (cd "${BUILD}/saves" && tar -cf - .) | (cd "${OUT}/Memory Card" && tar -xkf - 2>/dev/null) || true
fi
# Mods: what the build ships replaces the same files; anything else stays.
if [[ -d "${BUILD}/mods" ]]; then
  (cd "${BUILD}/mods" && tar -cf - .) | (cd "${OUT}/Mods" && tar -xf -)
fi

mkdir -p "${DATA}/licenses"
cp "${ROOT}/psxrecomp/LICENSE" "${DATA}/licenses/psxrecomp-LICENSE.txt"
cp "${ROOT}/psxrecomp/THIRD_PARTY_ATTRIBUTION.md" "${DATA}/licenses/THIRD_PARTY_ATTRIBUTION.md"
cp "${ROOT}/recomp-ui/LICENSE" "${DATA}/licenses/recomp-ui-LICENSE.txt"
cat >"${DATA}/README.txt" <<'EOF'
Armored Core: Master of Arena

Start "Armored Core Moa.exe" in the folder above this one.

  Armored Core Moa.exe   starts the game
  (disc image)           put your own Master of Arena disc image (.cue + .bin,
                         .chd or .iso) next to Armored Core Moa.exe; it is
                         found by itself
  Memory Card\           memory cards
  Mods\                  mods; put new mod packages in Mods\new and install
                         them from the MODS page
  RecompData\            the game's own files (this folder)

The whole folder can be moved or copied to another PC as it is. No disc
image is included.
EOF
echo "Packaged ${OUT}"
