#!/usr/bin/env python3
"""Marks the architectural state of generated whole-AOT functions __restrict.

  aot_restrict_state.py AOT_CPP_DIR [--check]

Generated functions reach the guest registers through references into their
`Oot3dAotArchitecturalState& state`. A guest memory store is a byte copy that
may alias anything, so after each one the compiler reloaded every register
from memory (over a third of the time in the generated code, 1.48 profile).
Copying the registers into locals instead (the generator's local_state mode,
tried in 1.49) wrote them back and read them again at every call and exit
and was slower on the console, as upstream had found on the desktop.

__restrict says what holds here: within a call, the state object is only
reached through `state` (it is its own copy of the registers, not guest
memory, and no other pointer refers to it; callees receive it, which the
compiler accounts for). The compiler may then keep registers in host
registers across guest memory accesses and store them back only where the
state is passed on - with no added code. Idempotent; --check only reports.
"""

import re
import sys
from pathlib import Path

PARAMETER = re.compile(r"Oot3dAotArchitecturalState& state(?=[,)])")


def main() -> int:
    directory = Path(sys.argv[1])
    check = "--check" in sys.argv[2:]
    count = files = 0
    for path in sorted(list(directory.glob("oot3d_whole_aot_*.cpp")) +
                       list(directory.glob("oot3d_whole_aot_*.h"))):
        source = path.read_text(encoding="utf-8")
        result, changed = PARAMETER.subn("Oot3dAotArchitecturalState& __restrict state", source)
        if changed == 0:
            continue
        files += 1
        count += changed
        if not check:
            path.write_text(result, encoding="utf-8")
    print(f"{'would mark' if check else 'marked'} {count} state parameters __restrict in {files} files")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
