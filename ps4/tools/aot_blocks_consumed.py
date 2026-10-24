#!/usr/bin/env python3
"""Reads the whole-AOT consumed block count through Oot3dAotBlocksConsumed.

  aot_blocks_consumed.py AOT_CPP_DIR [--check]

On the PS4 a block entry steps only BlocksRemaining; the consumed count is
the context's starting budget minus it (Oot3dAotBlocksConsumed,
oot3d_native_whole_aot_runtime.h), and BlocksConsumed is no longer kept up to
date. The generator now reads the count that way (whole_aot_cpp.py); this
applies the same text to the existing oot3d_whole_aot_generated.cpp.
Idempotent; --check only reports.
"""

import sys
from pathlib import Path

REPLACEMENTS = (
    ("        *blocksConsumed = context.BlocksConsumed != 0U\n"
     "                              ? context.BlocksConsumed : 1U;\n",
     "        const uint32_t consumed = Oot3dAotBlocksConsumed(context);\n"
     "        *blocksConsumed = consumed != 0U ? consumed : 1U;\n"),
    ("                   a32::FallbackReason::None, context.BlocksConsumed};\n",
     "                   a32::FallbackReason::None,\n"
     "                   Oot3dAotBlocksConsumed(context)};\n"),
)


def main() -> int:
    if len(sys.argv) not in (2, 3) or (len(sys.argv) == 3 and sys.argv[2] != "--check"):
        print(__doc__.strip(), file=sys.stderr)
        return 2
    path = Path(sys.argv[1]) / "oot3d_whole_aot_generated.cpp"
    text = path.read_text()
    pending = sum(text.count(old) for old, _ in REPLACEMENTS)
    if pending == 0:
        if "context.BlocksConsumed" in text:
            print("unknown BlocksConsumed use left; not changed", file=sys.stderr)
            return 1
        print("consumed count already read through Oot3dAotBlocksConsumed")
        return 0
    if len(sys.argv) == 3:
        print(f"{pending} reads of context.BlocksConsumed to replace")
        return 1
    for old, new in REPLACEMENTS:
        text = text.replace(old, new)
    if "context.BlocksConsumed" in text:
        print("unknown BlocksConsumed use left; not changed", file=sys.stderr)
        return 1
    path.write_text(text)
    print(f"replaced {pending} reads of context.BlocksConsumed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
