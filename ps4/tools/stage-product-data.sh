#!/usr/bin/env bash
# Uploads the user's own TriAevum data (a desktop data/ directory prepared by
# Forge, e.g. refs/data) to the console for the product (TRIA00005) over the
# GoldHEN FTP server. Nothing here is shipped in the PKG: it is the user's
# configuration and the TopScreen pack Forge imported from their ROM.
#   stage-product-data.sh DATA_DIR [--with-save]
#     DATA_DIR/config/{TriAevum,topscreen_ui,oot3d_controls,game_language,savestates}.json
#     DATA_DIR/mods/topscreen/<hash>/{atlas_overrides.o3tu,zelda_link_ext.zar}
#     DATA_DIR/savedata/save00.bin            (only with --with-save)
# The game itself (/data/triaevum/game: process-manifest.json + inputs/) is
# the module host's staging and stays as it is.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
source "$HERE/env.sh"
data="${1:?usage: stage-product-data.sh DATA_DIR [--with-save]}"
withSave="${2:-}"
ftp="ftp://${PS4_HOST}:${PS4_FTP_PORT}/data/triaevum"

put() {  # local remote
    echo "  $1 -> /data/triaevum/$2"
    curl -sS --ftp-create-dirs -T "$1" "$ftp/$2"
}

for name in TriAevum topscreen_ui oot3d_controls game_language savestates; do
    [ -f "$data/config/$name.json" ] && put "$data/config/$name.json" "config/$name.json"
done
pack="$(ls -d "$data"/mods/topscreen/*/ 2>/dev/null | head -1)"
[ -n "$pack" ] || { echo "no TopScreen pack under $data/mods/topscreen" >&2; exit 1; }
put "$pack/atlas_overrides.o3tu" "mods/topscreen/atlas_overrides.o3tu"
put "$pack/zelda_link_ext.zar" "mods/topscreen/zelda_link_ext.zar"
if [ "$withSave" = --with-save ] && [ -f "$data/savedata/save00.bin" ]; then
    put "$data/savedata/save00.bin" "savedata/save00.bin"
fi
echo "done"
