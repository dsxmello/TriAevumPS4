#!/usr/bin/env python3
"""Lists the build closure of a TriAevum CMake target from the CMake File API
(codemodel-v2) of a host configure: every dependent target with its kind,
sources, compile definitions and include directories. The PS4 product build
consumes this instead of re-deriving TriAevum's CMake graph by hand.
  closure.py BUILD_DIR TARGET [--json OUT]"""
import json, os, sys

build, target = sys.argv[1], sys.argv[2]
reply = os.path.join(build, ".cmake/api/v1/reply")
index = max((f for f in os.listdir(reply) if f.startswith("index-")), default=None)
cm_file = json.load(open(os.path.join(reply, index)))["reply"]["codemodel-v2"]["jsonFile"]
cm = json.load(open(os.path.join(reply, cm_file)))
cfg = cm["configurations"][0]
by_id = {t["id"]: t for t in cfg["targets"]}
by_name = {t["name"]: t["id"] for t in cfg["targets"]}
details = {}
def load(tid):
    if tid not in details:
        details[tid] = json.load(open(os.path.join(reply, by_id[tid]["jsonFile"])))
    return details[tid]
order, seen = [], set()
def visit(tid):
    if tid in seen: return
    seen.add(tid)
    for dep in load(tid).get("dependencies", []):
        visit(dep["id"])
    order.append(tid)
visit(by_name[target])
src_root = cm["paths"]["source"]
out = []
for tid in order:
    d = load(tid)
    if d["type"] in ("UTILITY", "INTERFACE_LIBRARY"):
        continue
    groups = d.get("compileGroups", [])
    entry = {"name": d["name"], "type": d["type"], "sources": [], "groups": []}
    for gi, g in enumerate(groups):
        entry["groups"].append({
            "language": g.get("language"),
            "defines": [x["define"] for x in g.get("defines", [])],
            "includes": [x["path"] for x in g.get("includes", [])],
            "flags": [x["fragment"] for x in g.get("compileCommandFragments", [])],
        })
    for s in d.get("sources", []):
        if "compileGroupIndex" not in s: continue
        p = s["path"]
        if not os.path.isabs(p): p = os.path.join(src_root, p)
        entry["sources"].append({"path": p, "group": s["compileGroupIndex"]})
    out.append(entry)
if "--json" in sys.argv:
    json.dump(out, open(sys.argv[sys.argv.index("--json") + 1], "w"), indent=1)
total = 0
for e in out:
    total += len(e["sources"])
    print(f'{e["type"]:16} {len(e["sources"]):5}  {e["name"]}')
print("total sources", total)
