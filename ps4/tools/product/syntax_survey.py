#!/usr/bin/env python3
"""Compiles every source of a closure.py JSON with the PS4 toolchain in
-fsyntax-only mode and buckets the failures by their first error, so porting
work can be sized and ordered.  syntax_survey.py CLOSURE_JSON OUT_DIR [extra flags...]"""
import concurrent.futures, json, os, re, shlex, subprocess, sys

closure = json.load(open(sys.argv[1]))
out_dir = sys.argv[2]
extra = sys.argv[3:]
os.makedirs(out_dir, exist_ok=True)
root = os.environ["PS4_PORT_ROOT"]
sysroot = os.environ["PS4_SYSROOT"]
oo = os.environ["OPENORBIS"]
base = ["clang++-21", "-target", "x86_64-pc-freebsd12-elf", "-nostdinc++",
        "-isystem", f"{sysroot}/include/c++/v1", "-D_GNU_SOURCE", "-D__PS4__",
        "-D__OPENORBIS__", "-D__ORBIS__", "-DPS4", "-D__BSD_VISIBLE", "-D_BSD_SOURCE",
        "-fPIC", "-isysroot", oo, "-isystem", f"{sysroot}/libc-overlay", "-isystem",
        f"{oo}/include", f"-I{oo}/usr/include", "-march=btver2", "-fsyntax-only",
        "-w", "-ferror-limit=3"]
cbase = [a if a != "clang++-21" else "clang-21" for a in base if a not in ("-nostdinc++",)]
jobs = []
for t in closure:
    for s in t["sources"]:
        g = t["groups"][s["group"]]
        lang = g["language"]
        if lang not in ("C", "CXX"): continue
        cmd = (base + ["-std=gnu++20"]) if lang == "CXX" else (cbase[:] + ["-std=gnu17"])
        if lang == "C":
            cmd = [c for c in cmd if c not in ("-isystem", f"{sysroot}/include/c++/v1")] if False else cmd
            cmd = ["clang-21"] + [a for a in cmd[1:] if a != "-nostdinc++"]
            # drop libc++ include pair for C
            fixed = []
            skip = False
            for a in cmd:
                if skip: skip = False; continue
                if a == "-isystem" and False: pass
                fixed.append(a)
            cmd = fixed
        cmd += [f"-D{d}" for d in g["defines"]] + [f"-I{i}" for i in g["includes"]] + extra
        jobs.append((t["name"], s["path"], cmd + [s["path"]]))
def run(job):
    name, path, cmd = job
    r = subprocess.run(cmd, capture_output=True, text=True)
    first = ""
    for line in r.stderr.splitlines():
        if " error: " in line or "fatal error:" in line:
            first = line; break
    return name, path, r.returncode, first, r.stderr
results = []
with concurrent.futures.ThreadPoolExecutor(os.cpu_count()) as pool:
    for res in pool.map(run, jobs):
        results.append(res)
ok = sum(1 for r in results if r[2] == 0)
print(f"ok {ok}/{len(results)}")
buckets = {}
for name, path, rc, first, err in results:
    if rc == 0: continue
    key = re.sub(r"^.*?(fatal error|error): ", "", first)
    key = re.sub(r"'[^']*'", "'X'", key) if "file not found" not in key else key
    buckets.setdefault(key, []).append((name, path))
    with open(os.path.join(out_dir, os.path.basename(path) + ".err"), "w") as f:
        f.write(" ".join(shlex.quote(c) for c in [])+err)
for key, items in sorted(buckets.items(), key=lambda kv: -len(kv[1])):
    print(f"{len(items):4}  {key[:150]}")
    for name, path in items[:3]:
        print(f"        {name}: {os.path.relpath(path, root + '/..')}")
