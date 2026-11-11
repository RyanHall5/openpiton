#!/usr/bin/env python3
"""Fail if the reference doc is missing anything users can see.

Checks that rit/docs/noctrace_reference.md mentions (in `backticks`):
  * every noctrace command-line option and every choice value
  * every plusarg the trace monitor reads
  * every output file noctrace writes, and every column of every CSV
  * every summary.json key

    usage: check_docs.py RIT_DIR TRACE.csv
"""
import json
import os
import re
import subprocess
import sys
import tempfile

import pandas as pd

rit, trace = sys.argv[1], sys.argv[2]
ref = open(os.path.join(rit, "docs", "noctrace_reference.md")).read()
# fenced ``` blocks would break backtick pairing; inline `spans` are what count
inline = re.sub(r"```.*?```", "", ref, flags=re.S)
documented = set(re.findall(r"`([^`\n]+)`", inline))
# also accept items listed inside one backtick span, e.g. `a, b, c` or `x / y`
for span in list(documented):
    documented.update(t.strip() for t in re.split(r"[,/ |]+", span) if t.strip())

missing = []


def need(item, what):
    if item not in documented:
        missing.append(f"{what}: {item}")


# CLI options and choices
cli = open(os.path.join(rit, "noctrace", "cli.py")).read()
for opt in re.findall(r'add_argument\(\s*(?:"-\w",\s*)?"(--[\w-]+)"', cli):
    need(opt, "CLI option")
for choices in re.findall(r"choices=\[([^\]]+)\]", cli):
    for c in re.findall(r'"([^"]+)"', choices):
        need(c, "CLI choice")

# monitor plusargs
mon = open(os.path.join(rit, "..", "piton", "verif", "env", "manycore",
                        "noc_trace_mon.v.pyv")).read()
for pa in re.findall(r'\$test\$plusargs\("(\w+)"\)', mon):
    need("+" + pa, "plusarg")
for pa in re.findall(r'\$value\$plusargs\("(\w+)=', mon):
    need(f"+{pa}=NAME", "plusarg")

# output files and columns (two traces so runs_summary.csv exists too)
with tempfile.TemporaryDirectory() as out:
    env = dict(os.environ, PYTHONPATH=rit + os.pathsep + os.environ.get("PYTHONPATH", ""))
    subprocess.run([sys.executable, "-m", "noctrace", trace, trace, "--name", "a", "--name", "b",
                    "--export-events", "--app", "x", "--round", "1", "-o", out],
                   check=True, env=env, stdout=subprocess.DEVNULL)
    need("runs_summary.csv", "output file")
    run = os.path.join(out, "a")
    for root, _, files in os.walk(run):
        for f in files:
            rel = os.path.relpath(os.path.join(root, f), run)
            if rel.startswith("signatures" + os.sep) and rel.endswith(".png"):
                continue  # documented as signatures/core<N>.png, signatures/total.png
            need(f, "output file")
            if f.endswith((".csv", ".csv.gz")) and f != "path_matrix.csv":
                for col in pd.read_csv(os.path.join(root, f), nrows=1).columns:
                    need(col, f"column of {f}")
    s = json.load(open(os.path.join(run, "summary.json")))
    for k in s:
        need(k, "summary.json key")
    for k in next(iter(s["per_noc"].values())):
        need(k, "summary.json per_noc key")
    if "busiest_mesh_link" in s:
        for k in s["busiest_mesh_link"]:
            need(k, "summary.json busiest_mesh_link key")

if missing:
    print("DOCS FAIL: not mentioned in rit/docs/noctrace_reference.md:")
    for m in missing:
        print("   ", m)
    sys.exit(1)
print("PASS docs: every option, plusarg, output file, column and summary key is documented")
