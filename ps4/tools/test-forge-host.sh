#!/usr/bin/env bash
# Checks the console Forge (ports/ps4-product/src/forge) on the PC against the
# desktop Forge: prepares the game from a decrypted ROM with the host build of
# forge_core, then compares the process manifest (byte for byte) and the RomFS
# fingerprint with the Python originals.
#   test-forge-host.sh ROM [WORK_DIR]
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
PORT="$(cd "$HERE/.." && pwd)"
TRIAEVUM="$(cd "$PORT/.." && pwd)"
[ -d "$TRIAEVUM/tools/oot3d" ] || TRIAEVUM="$(cd "$PORT/../TriAevum" && pwd)"
rom="${1:?usage: test-forge-host.sh ROM [WORK_DIR]}"
work="${2:-$(mktemp -d)}"
mkdir -p "$work"
src="$PORT/ports/ps4-product"
clang++ -std=c++20 -O2 -Wall -Wextra -I"$src/src" -I"$PORT/ports/ps4-vk-game/third_party" \
    "$src/src/forge/forge_core.cpp" "$src/src/forge/forge_sha256.cpp" \
    "$src/tests/forge_host_test.cpp" -o "$work/forge_host_test"
rm -rf "$work/work" "$work/game"
"$work/forge_host_test" "$rom" "$PORT/forge/recipes/oot3d.json" "$PORT/forge" \
    "$work/work" "$work/game" > "$work/report.json"
( cd "$TRIAEVUM" && python3 -I - "$work/game" "$work/report.json" "$PORT/forge/recipes/oot3d.json" <<'E'
import json, sys
sys.path.insert(0, '.')
from pathlib import Path
from tools.oot3d.native_a32_runtime.build_process_manifest import build_manifest
from tools.triaevum_release.romfs_identity import fingerprint
game, report = Path(sys.argv[1]), json.loads(Path(sys.argv[2]).read_text())
recipe = next(r for r in json.loads(Path(sys.argv[3]).read_text())['recipes'] if r['id'] == report['recipe'])
manifest = build_manifest(game/'inputs/exheader.bin', game/'inputs/code.bin', game/'inputs/romfs.bin')
manifest['source'].update(exheader_path='inputs/exheader.bin', code_bin_path='inputs/code.bin',
                          romfs_image_path='inputs/romfs.bin')
python = (json.dumps(manifest, indent=2, sort_keys=True) + '\n').encode()
checks = {
    'inputs match the recipe': report['inputs'] == recipe['inputs'],
    'manifest identical to build_process_manifest.py': python == (game/'process-manifest.json').read_bytes(),
    'RomFS fingerprint identical to romfs_identity.py': fingerprint(game/'inputs/romfs.bin') == report['romfs_fingerprint'],
}
print(f"recipe {report['recipe']} ({report['seconds']:.1f} s)")
for name, ok in checks.items():
    print(f"  {'ok  ' if ok else 'FAIL'} {name}")
sys.exit(0 if all(checks.values()) else 1)
E
)
