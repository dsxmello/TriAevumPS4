#!/usr/bin/env python3
"""Batches the A32 LDM/STM accesses of already generated whole-AOT shards.

  aot_batch_block_transfers.py AOT_CPP_DIR [--check]

The PS4 port keeps the generated title sources (title-content/aot-cpp) but
not the generator's inputs, so the batching the generator now emits
(tools/oot3d/native_a32_runtime/whole_aot_cpp.py _emit_block_transfer) is
applied to the existing shards here, with the same shape:

  store: const uint32_t words[N] = {...};
         if (!MEM.WriteWordsFast(firstAddress, words)) { <the per-word code> }
  load:  uint32_t valueR = 0; ...
         uint32_t words[N];
         if (MEM.ReadWordsFast(firstAddress, words)) { valueR = words[i]; ... }
         else { <the per-word code> }

The per-word code is the original text, only re-indented: whenever the
batched path declines (page boundary, slow page, traced writes, a memory
region) execution is exactly the previous one. Idempotent: a block already
batched is left alone. --check only reports what it would change.
"""

import re
import sys
from pathlib import Path

LOAD = re.compile(
    r"(?P<ind>[ ]*)uint32_t value(?P<reg>\d+) = 0;\n"
    r"(?P=ind)if \(!(?P<mem>context\.Memory|regionMemory)\.ReadFast<uint32_t>\(\n"
    r"[ ]*firstAddress \+ (?P<k>\d+)U, &value(?P=reg)\)\) \{\n"
    r"(?P<fail>(?:(?P=ind)    .*\n)+?)"
    r"(?P=ind)\}\n")
STORE = re.compile(
    r"(?P<ind>[ ]*)if \(!(?P<mem>context\.Memory|regionMemory)\.WriteFast<uint32_t>\(\n"
    r"[ ]*firstAddress \+ (?P<k>\d+)U, (?P<expr>[^\n]+?)\)\) \{\n"
    r"(?P<fail>(?:(?P=ind)    .*\n)+?)"
    r"(?P=ind)\}\n")
FIRST = re.compile(r"\n(?P<ind>[ ]*)const uint32_t firstAddress = [^\n]+;\n")


def indent(text: str, by: str) -> str:
    return "".join(by + line if line.strip() else line for line in text.splitlines(True))


def batch_at(source: str, start: int, ind: str):
    """The batched text for the word sequence at `start`, and its end."""
    units, position, pattern = [], start, None
    for candidate in (LOAD, STORE):
        match = candidate.match(source, position)
        if match and match.group("ind") == ind:
            pattern = candidate
            break
    if pattern is None:
        return None
    while True:
        match = pattern.match(source, position)
        if not match or match.group("ind") != ind or int(match.group("k")) != 4 * len(units) or (
                units and match.group("mem") != units[0].group("mem")):
            break
        units.append(match)
        position = match.end()
    if len(units) < 2:
        return None
    count, memory = len(units), units[0].group("mem")
    original = source[start:position]
    if pattern is STORE:
        words = ", ".join(unit.group("expr") for unit in units)
        text = (f"{ind}{{\n{ind}    const uint32_t words[{count}] = {{{words}}};\n"
                f"{ind}    if (!{memory}.WriteWordsFast(firstAddress, words)) {{\n"
                f"{indent(original, '        ')}{ind}    }}\n{ind}}}\n")
    else:
        declarations = "".join(f"{ind}uint32_t value{unit.group('reg')} = 0;\n" for unit in units)
        assigns = "".join(f"{ind}        value{unit.group('reg')} = words[{i}];\n"
                          for i, unit in enumerate(units))
        per_word = "".join(source[unit.start():unit.end()].split("\n", 1)[1] for unit in units)
        text = (f"{declarations}{ind}{{\n{ind}    uint32_t words[{count}];\n"
                f"{ind}    if ({memory}.ReadWordsFast(firstAddress, words)) {{\n{assigns}"
                f"{ind}    }} else {{\n{indent(per_word, '        ')}{ind}    }}\n{ind}}}\n")
    return text, position, "store" if pattern is STORE else "load", count


def transform(source: str):
    out, last, stats = [], 0, {"load": 0, "store": 0, "words": 0}
    for first in FIRST.finditer(source):
        if first.start() < last:
            continue
        result = batch_at(source, first.end(), first.group("ind"))
        if result is None:
            continue
        text, end, kind, count = result
        out.append(source[last:first.end()])
        out.append(text)
        last = end
        stats[kind] += 1
        stats["words"] += count
    out.append(source[last:])
    return "".join(out), stats


def main() -> int:
    directory = Path(sys.argv[1])
    check = "--check" in sys.argv[2:]
    totals = {"load": 0, "store": 0, "words": 0, "files": 0}
    for path in sorted(directory.glob("oot3d_whole_aot_*.cpp")):
        source = path.read_text(encoding="utf-8")
        result, stats = transform(source)
        if result == source:
            continue
        totals["files"] += 1
        for key in ("load", "store", "words"):
            totals[key] += stats[key]
        if not check:
            path.write_text(result, encoding="utf-8")
    print(f"{'would batch' if check else 'batched'} {totals['load']} LDM and {totals['store']} STM "
          f"blocks ({totals['words']} words) in {totals['files']} files")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
