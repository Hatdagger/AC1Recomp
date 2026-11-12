#!/usr/bin/env bash
# Build the recompiler tools (psxrecomp-game, psxrecomp-bios) that the
# compiler bundle needs (see psxrecomp/recompiler/tools-compat/README.md).
# Usage: scripts/build_recompiler_tools.sh [out-dir]     (default build-recompiler)
# The tools land in <out-dir>/; the working copy of the tree in <out-dir>-src/.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${1:-${ROOT}/build-recompiler}"
SRC="${OUT}-src"
JOBS="${BUILD_JOBS:-$(nproc 2>/dev/null || echo 4)}"
cd "${ROOT}"

rm -rf "${SRC}"
mkdir -p "${SRC}" "${OUT}"
tar -cf - --exclude=psxrecomp/overlay_toolchain --exclude=psxrecomp/recompiler/tools-compat \
    --exclude=psxrecomp/generated psxrecomp | (cd "${SRC}" && tar -xf -)
(cd psxrecomp/recompiler/tools-compat && tar -cf - recompiler runtime) | (cd "${SRC}/psxrecomp" && tar -xf -)

EXTRA=()
case "$(uname -s)" in
  MINGW*|MSYS*|CYGWIN*) EXTRA+=(-DPSXRECOMP_STATIC_CLI=ON) ;;
esac
cmake -S "${SRC}/psxrecomp/recompiler" -B "${OUT}" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF "${EXTRA[@]}"
cmake --build "${OUT}" --target psxrecomp-game psxrecomp-bios -j"${JOBS}"

EXT=""
[[ -f "${OUT}/psxrecomp-game.exe" ]] && EXT=".exe"
for tool in psxrecomp-game psxrecomp-bios; do
  [[ -f "${OUT}/${tool}${EXT}" ]] || { echo "error: ${OUT}/${tool}${EXT} was not built" >&2; exit 1; }
done

# The tools must carry the same code-generation hash as the game, or the game
# will never load what they compile.
TOOL_HASH="$("${OUT}/psxrecomp-game${EXT}" --codegen-hash | tr -d '[:space:]' | tr 'A-F' 'a-f')"
HDR="${ROOT}/psxrecomp/runtime/include/overlay_codegen_hash.h"
if [[ -f "${HDR}" ]]; then
  GAME_HASH="$(sed -n 's/^#define[[:space:]]*PSX_OVERLAY_CODEGEN_HASH[[:space:]]*0x\([0-9A-Fa-f]*\).*/\1/p' "${HDR}" | tr 'A-F' 'a-f')"
  if [[ "${TOOL_HASH}" != "${GAME_HASH}" ]]; then
    echo "error: recompiler tools hash ${TOOL_HASH} != game hash ${GAME_HASH}" >&2
    exit 1
  fi
  echo "recompiler tools match the game: codegen hash ${TOOL_HASH}"
else
  echo "recompiler tools codegen hash ${TOOL_HASH} (game not built yet; not compared)"
fi
