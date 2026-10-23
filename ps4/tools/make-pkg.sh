#!/usr/bin/env bash
# Package a staged app directory into a PS4 fake-PKG.
# Adapted from DolphinPS4's scripts/make-pkg.sh (same SFO/PkgTool pipeline).
#
#   make-pkg.sh <stage-dir> <TITLE_ID> <title> <version XX.YY> <content-label> <out-dir>
#
# <stage-dir> must contain eboot.bin and sce_sys/icon0.png; anything else in it
# (sce_module/, assets, ...) is packaged as-is.
#   SFO_STYLE=plain     Vulkan/GNM identity: CATEGORY gd, APP_TYPE 1 (default here)
#   SFO_STYLE=retroarch Piglet identity: CATEGORY gde, FORMAT obs (the old app)
#   NO_PIGLET_MODULES=1 skip the sample Piglet .prx (Vulkan-only apps don't need them)
set -euo pipefail
source "$(dirname "$0")/env.sh"
HERE="$DOLPHINPS4/scripts"

STAGE="$(readlink -f "$1")"; TITLE_ID="$2"; TITLE="$3"; VERSION="$4"; LABEL="$5"; OUT="$(readlink -f "$6")"
TOOLS="$OPENORBIS/bin/linux"
export DOTNET_SYSTEM_GLOBALIZATION_INVARIANT=1

[[ "$TITLE_ID" =~ ^[A-Z]{4}[0-9]{5}$ ]] || { echo "TITLE_ID must look like ABCD12345" >&2; exit 1; }
[[ "$VERSION" =~ ^[0-9]{2}\.[0-9]{2}$ ]] || { echo "version must look like 01.00" >&2; exit 1; }
[ -f "$STAGE/eboot.bin" ] || { echo "missing $STAGE/eboot.bin" >&2; exit 1; }
[ -f "$STAGE/sce_sys/icon0.png" ] || { echo "missing $STAGE/sce_sys/icon0.png" >&2; exit 1; }

mkdir -p "$STAGE/sce_module"
[ "${NO_RIGHT_SPRX:-0}" = 1 ] || { mkdir -p "$STAGE/sce_sys/about"; cp "$OPENORBIS/samples/piglet/sce_sys/about/right.sprx" "$STAGE/sce_sys/about/"; }
if [ "${NO_PIGLET_MODULES:-0}" != 1 ]; then
    cp "$OPENORBIS/samples/piglet/sce_module/"*.prx "$STAGE/sce_module/"
fi

LABEL="$(printf '%s' "$LABEL" | tr '[:lower:]' '[:upper:]' | tr -cd 'A-Z0-9')"
LABEL="$(printf '%-16s' "${LABEL:0:16}" | tr ' ' '0')"
CONTENT_ID="IV0000-${TITLE_ID}_00-${LABEL}"

SFO="$STAGE/sce_sys/param.sfo"
rm -f "$SFO"
"$TOOLS/PkgTool.Core" sfo_new "$SFO"
set_entry() { "$TOOLS/PkgTool.Core" sfo_setentry "$SFO" "$1" --type "$2" --maxsize "$3" --value "$4" >/dev/null; }
if [ "${SFO_STYLE:-plain}" = retroarch ]; then
    # RetroArch for PS4's launch parameters: the shell's Piglet only gives an EGL
    # display to processes with these values (the old TriAevum GLES app used them).
    set_entry APP_TYPE Integer 4 0
    set_entry ATTRIBUTE Integer 4 0x20814016
    set_entry ATTRIBUTE2 Integer 4 0x6
    set_entry CATEGORY Utf8 4 gde
    set_entry FORMAT Utf8 4 obs
    set_entry SYSTEM_VER Integer 4 0x3fc
else
    # Plain OpenOrbis values: known to launch with the default PAID and gives the
    # process its own VideoOut (required for RADV on GNM; 4.5 GiB direct memory).
    set_entry APP_TYPE Integer 4 1
    set_entry ATTRIBUTE Integer 4 0
    set_entry CATEGORY Utf8 4 gd
    set_entry SYSTEM_VER Integer 4 0
fi
set_entry APP_VER Utf8 8 "$VERSION"
set_entry CONTENT_ID Utf8 48 "$CONTENT_ID"
set_entry DOWNLOAD_DATA_SIZE Integer 4 0
set_entry TITLE Utf8 128 "$TITLE"
set_entry TITLE_ID Utf8 12 "$TITLE_ID"
set_entry VERSION Utf8 8 "$VERSION"

rm -f "$STAGE/pkg.gp4"
( cd "$STAGE" && "$TOOLS/create-gp4" -out pkg.gp4 --content-id="$CONTENT_ID" --path . ) >/dev/null
mkdir -p "$OUT"
( cd "$STAGE" && "$TOOLS/PkgTool.Core" pkg_build pkg.gp4 "$OUT" ) >/dev/null
rm -f "$STAGE/pkg.gp4"
echo "$OUT/$CONTENT_ID.pkg"
