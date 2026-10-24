#!/usr/bin/env python3
"""Puts a dispatch cache in front of the generated whole-AOT dispatch search.

  aot_dispatch_cache.py AOT_CPP_DIR [--check]

FindDispatchEntry (oot3d_whole_aot_generated.cpp) binary-searches a table of
162,757 entries (2.6 MB) on every function return, branch and indirect call;
its probes missed the cache and were 1.3% of the game thread in heavy areas
(1.48 profile). The generator now emits a per-thread direct-mapped cache of
pc -> entry index in front of the search (whole_aot_cpp.py, after
"const DispatchEntry* SearchDispatchEntry"); this applies the same text to
the existing generated file. Idempotent; --check only reports.
"""

import re
import sys
from pathlib import Path

SEARCH = "const DispatchEntry* FindDispatchEntry(uint32_t pc) noexcept {\n"
END = re.compile(r"\n    return found != end && found->Pc == pc\n"
                 r"               \? &\*found\n"
                 r"               : nullptr;\n}\n")
CACHE = """
// Function returns, branches and indirect calls dispatch here; the
// search's probes into the large entry table missed the cache (most of
// the dispatcher's time on the PS4). A small per-thread direct-mapped
// cache of pc -> entry index answers repeats; Key is ~pc so that the
// zeroed table matches nothing (no A32 pc is 0xFFFFFFFF).
struct DispatchCacheSlot {
    uint32_t Key = 0U;
    uint32_t Index = 0U;
};
constexpr uint32_t kDispatchCacheLog2 = 10U;
constexpr uint32_t kDispatchCacheMissing = 0xFFFFFFFFU;
thread_local std::array<DispatchCacheSlot, size_t{1} << kDispatchCacheLog2>
    tDispatchCache{};

const DispatchEntry* FindDispatchEntry(uint32_t pc) noexcept {
    DispatchCacheSlot& slot =
        tDispatchCache[((pc >> 2U) * 2654435761U) >> (32U - kDispatchCacheLog2)];
    if (slot.Key == ~pc) {
        return slot.Index == kDispatchCacheMissing
                   ? nullptr
                   : &kDispatchEntries[slot.Index];
    }
    const DispatchEntry* found = SearchDispatchEntry(pc);
    slot.Key = ~pc;
    slot.Index = found == nullptr
                     ? kDispatchCacheMissing
                     : static_cast<uint32_t>(found - kDispatchEntries.data());
    return found;
}
"""


def main() -> int:
    path = Path(sys.argv[1]) / "oot3d_whole_aot_generated.cpp"
    check = "--check" in sys.argv[2:]
    source = path.read_text(encoding="utf-8")
    if "SearchDispatchEntry" in source:
        print("dispatch cache already present")
        return 0
    start = source.index(SEARCH)
    end = END.search(source, start)
    if end is None:
        raise SystemExit("FindDispatchEntry has an unexpected shape")
    result = (source[:start] +
              SEARCH.replace("FindDispatchEntry", "SearchDispatchEntry") +
              source[start + len(SEARCH):end.end()] + CACHE +
              source[end.end():])
    if not check:
        path.write_text(result, encoding="utf-8")
    print(f"{'would add' if check else 'added'} the dispatch cache to {path.name}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
