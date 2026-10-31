#!/usr/bin/env python3
"""Checks lang/*.json against lang/en_US.json (the master).

A pack may leave strings out (they stay English) but may not invent keys, and every string must
keep the same [Button] names as the English one, because the program shows them as they are. The
four direction names ([Up], [Down], [Left], [Right]) are translated on purpose, so only the
buttons that have a label of their own (A, B, X, Y, Start, Select, L1, R1) are compared.
Run:  python3 tools/check_lang.py"""
import glob, json, os, re, sys

root = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "lang")
master = json.load(open(os.path.join(root, "en_US.json"), encoding="utf-8"))
BTN = re.compile(r"\[[A-Za-z0-9 +/]+\]")
REAL = {"[A]", "[B]", "[X]", "[Y]", "[Start]", "[Select]", "[L1]", "[R1]", "[Select + Start]"}


def buttons(text):
    return sorted(t for t in BTN.findall(text) if t in REAL)
bad = 0
for path in sorted(glob.glob(os.path.join(root, "*.json"))):
    loc = os.path.basename(path)[:-5]
    if loc == "en_US":
        continue
    pack = json.load(open(path, encoding="utf-8"))
    for k, v in pack.items():
        if k.startswith("_"):
            continue
        if k not in master:
            print("%s: unknown key %s" % (loc, k)); bad += 1
        elif buttons(v) != buttons(master[k]):
            print("%s: %s: button names %s should be %s" % (loc, k, buttons(v), buttons(master[k]))); bad += 1
    missing = [k for k in master if not k.startswith("_") and k not in pack]
    print("%s: %d of %d translated%s" % (loc, len(master) - len(missing) - sum(k.startswith("_") for k in master), len([k for k in master if not k.startswith("_")]), "" if not missing else ", %d stay English" % len(missing)))
sys.exit(1 if bad else 0)
