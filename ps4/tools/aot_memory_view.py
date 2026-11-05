#!/usr/bin/env python3
"""Routes the generated whole-AOT functions' guest memory accesses through a view.

  aot_memory_view.py AOT_CPP_DIR [--check]

Every generated function reached guest memory as `context.Memory.ReadFast<T>`
(and WriteFast, Read/WriteWordsFast, Load/StoreExclusive): the memory object
from the context, then its page table, then the page entry, then the data,
all reloaded after every guest store (a byte-pointer store may alias them).
NativeA32AotMemoryView (oot3d_native_a32_memory.h) keeps the memory object
and the page table in locals and, on the PS4, makes the guest accesses
invisible to alias analysis, ordered by a token. The generated code already
calls commitState before every call, hook, SVC and exit and reloadState after
every call and hook: the view's Commit and Reload go to the same places.

The view must stay out of the commitState/reloadState lambdas: they are
captured by reference and handed to the noinline block-entry slow path, and
a view they used would escape and live in memory. So the generated code calls
it next to them, and the block-entry and external-call helpers get overloads
that take the view and commit/reload only around their host code:

  NativeA32AotMemoryView memory{context.Memory};   (before commitState)
  commitState();   ->  memory.Commit(); commitState();
  reloadState();   ->  reloadState(); memory.Reload();
  Oot3dAotEnterBlock/CallExternal(..., commitState, reloadState)  ->  (..., memory)
  context.Memory.X  ->  memory.X

Every return of a generated function is preceded by commitState() (audited:
847,907 of 847,907), so no guest store is left without a Commit.

Idempotent. --check only reports.
"""

import re
import sys
from pathlib import Path

COMMIT = "    const auto commitState = [&]() noexcept {\n"
RELOAD = "    const auto reloadState = [&]() noexcept {\n"
VIEW = "    NativeA32AotMemoryView memory{context.Memory};\n"
ACCESS = re.compile(r"context\.Memory\.(ReadFast|WriteFast|ReadWordsFast|WriteWordsFast|"
                    r"LoadExclusive|StoreExclusive)\b")


def transform(text: str) -> tuple[str, int, int]:
    if VIEW in text:
        return text, 0, 0
    functions = text.count(COMMIT)
    if functions != text.count(RELOAD):
        raise SystemExit("commitState/reloadState counts differ")
    text = text.replace(COMMIT, VIEW + COMMIT)
    text = text.replace("commitState();", "memory.Commit(); commitState();")
    text = text.replace("reloadState();", "reloadState(); memory.Reload();")
    text = text.replace("commitState, reloadState)", "commitState, reloadState, memory)")
    text, accesses = ACCESS.subn(r"memory.\1", text)
    if "context.Memory." in text:
        leftover = re.search(r"context\.Memory\.\w+", text).group(0)
        raise SystemExit(f"unhandled guest memory call {leftover}")
    return text, functions, accesses


def main() -> int:
    directory = Path(sys.argv[1])
    check = "--check" in sys.argv[2:]
    functions = accesses = files = 0
    for path in sorted(directory.glob("oot3d_whole_aot_shard_*.cpp")):
        text = path.read_text()
        new, count, used = transform(text)
        if new == text:
            continue
        files += 1
        functions += count
        accesses += used
        if not check:
            path.write_text(new)
    verb = "would route" if check else "routed"
    print(f"{verb} {accesses} guest memory accesses of {functions} functions in {files} files"
          " through NativeA32AotMemoryView")
    return 0


if __name__ == "__main__":
    sys.exit(main())
