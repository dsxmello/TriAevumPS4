#!/usr/bin/env python3
"""Keeps the guest registers of already generated whole-AOT functions in locals.

  aot_promote_registers.py AOT_CPP_DIR [--check]

The generated functions bind each A32 register they use to the architectural
state by reference (`uint32_t& r4 = state.R[4];`): every guest instruction
then loads and stores memory, and after every guest memory store the
compiler reloads them, since a store through a byte pointer may alias them.
On the PS4 that was over a third of the time in the generated code (1.48
profile). The generator's local_state mode (whole_aot_cpp.py _render_function)
keeps them in locals instead; its commitState/reloadState lambdas, which the
generated code already calls at every return, call, external call, indirect
call and block-entry hook, then write them back and read them again.

This applies exactly that mode's shape to functions generated without it:

  uint32_t& rN = state.R[N];  ->  uint32_t rN = state.R[N];
  auto& flags = state.Flags;  ->  Oot3dAotLazyFlags flags = state.Flags;
  commitState = [&] { }       ->  state.R[N] = rN; ... state.Flags = flags;
  reloadState = [&] { }       ->  rN = state.R[N]; ... flags = state.Flags;

Functions already in local mode (callback-free regions, memory views) are
left alone. Idempotent. --check only reports.
"""

import re
import sys
from pathlib import Path

HEADER = re.compile(
    r"(?P<decls>(?:    uint32_t& r\d+ = state\.R\[\d+\];\n)+)"
    r"(?P<flags>    auto& flags = state\.Flags;\n)?"
    r"    const auto commitState = \[&\]\(\) noexcept \{\n    \};\n"
    r"    const auto reloadState = \[&\]\(\) noexcept \{\n    \};\n")


def promote(match: re.Match) -> str:
    registers = re.findall(r"uint32_t& r(\d+) = state\.R\[\1\];", match.group("decls"))
    flags = match.group("flags") is not None
    out = [f"    uint32_t r{index} = state.R[{index}];\n" for index in registers]
    if flags:
        out.append("    Oot3dAotLazyFlags flags = state.Flags;\n")
    out.append("    const auto commitState = [&]() noexcept {\n")
    out.extend(f"        state.R[{index}] = r{index};\n" for index in registers)
    if flags:
        out.append("        state.Flags = flags;\n")
    out.append("    };\n    const auto reloadState = [&]() noexcept {\n")
    out.extend(f"        r{index} = state.R[{index}];\n" for index in registers)
    if flags:
        out.append("        flags = state.Flags;\n")
    out.append("    };\n")
    return "".join(out)


def main() -> int:
    directory = Path(sys.argv[1])
    check = "--check" in sys.argv[2:]
    functions = files = 0
    for path in sorted(directory.glob("oot3d_whole_aot_*.cpp")):
        source = path.read_text(encoding="utf-8")
        result, count = HEADER.subn(promote, source)
        if count == 0:
            continue
        files += 1
        functions += count
        if not check:
            path.write_text(result, encoding="utf-8")
    print(f"{'would promote' if check else 'promoted'} the registers of {functions} functions "
          f"in {files} files")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
