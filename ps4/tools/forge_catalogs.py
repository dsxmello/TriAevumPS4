#!/usr/bin/env python3
"""Writes the console Forge's mod catalogs from the desktop Forge's own sources.

  forge_catalogs.py TRIAEVUM_ROOT RECIPES_JSON OUTPUT_DIR

ui_texture_pack.json: OoT3D Vanilla UI/HUD 3.2 (tools/triaevum_release/
ui_texture_pack.py VANILLA_UI_HUD) with the archive contract every revision
recipe accepts (optional_inputs.vanilla_ui_hud_3_2_archive). The console
reads it in src/forge/forge_textures.cpp; nothing of the pack itself is here.
"""

import json
import sys
from pathlib import Path


def main() -> int:
    root, recipes_path, output = (Path(arg) for arg in sys.argv[1:4])
    sys.path.insert(0, str(root))
    from tools.triaevum_release.ui_texture_pack import (  # noqa: E402
        COMMON_DIRECTORY, LANGUAGE_FOLDERS, LANGUAGES_DIRECTORY, TEXTURE_FOLDER,
        UI_ATLAS_HD_TEXTURE, VANILLA_UI_HUD as pack, prepared_path)

    recipes = json.loads(recipes_path.read_text(encoding="utf-8"))["recipes"]
    contracts = {json.dumps(recipe["optional_inputs"][pack.archive_kind], sort_keys=True)
                 for recipe in recipes if pack.archive_kind in recipe.get("optional_inputs", {})}
    if len(contracts) != 1:
        raise SystemExit(f"the recipes do not agree on one {pack.archive_kind} archive")
    catalog = {
        "format": "triaevum_ps4_ui_texture_pack_v1",
        "mod_id": pack.mod_id,
        "version": pack.version,
        "title": pack.title,
        "author": pack.author,
        "page": pack.page,
        "attribution": pack.attribution,
        "license": pack.license_name,
        "license_url": pack.license_url,
        "downloads": list(pack.downloads),
        "archive": json.loads(contracts.pop()),
        "archive_root": pack.archive_root,
        "texture_folder": TEXTURE_FOLDER,
        "common_directory": COMMON_DIRECTORY,
        "languages_directory": LANGUAGES_DIRECTORY,
        "language_folders": list(LANGUAGE_FOLDERS),
        "ui_atlas_hd": str(prepared_path(UI_ATLAS_HD_TEXTURE)),
        "members": [{"name": name, "prepared": str(prepared_path(name)), "bytes": size,
                     "sha256": digest} for name, (size, digest) in sorted(pack.members.items())],
    }
    output.mkdir(parents=True, exist_ok=True)
    (output / "ui_texture_pack.json").write_text(
        json.dumps(catalog, indent=1, ensure_ascii=False) + "\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
