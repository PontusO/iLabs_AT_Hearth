#!/usr/bin/env python3
"""check_decls.py - every hearth_port.h / mt_matter.h / mt_devtypes.h
declaration must have exactly one function definition in the matching
Silabs stub/port file. -fsyntax-only alone proves a stub compiles clean;
it says nothing about a missing or duplicated definition. This does."""
import re
import sys

CORE = "../../core/include"
PORT = "../../platform/silabs/port"
DECL_START = re.compile(r'^[A-Za-z_][A-Za-z0-9_ \*]*\(')


def names_from(header):
    names = []
    for line in open(header):
        if not DECL_START.match(line):
            continue
        head = line.split('(', 1)[0]
        tokens = re.findall(r'[A-Za-z_][A-Za-z0-9_]*', head)
        if tokens:
            names.append(tokens[-1])
    return names


def check(header, impl, label):
    names = names_from(header)
    lines = open(impl).read().splitlines()
    ok, problems = 0, []
    for name in names:
        pat = re.compile(r'^[a-zA-Z_].*\b' + re.escape(name) + r'\s*\(')
        defs = [l for l in lines if pat.match(l) and not l.rstrip().endswith(';')]
        if len(defs) == 1:
            ok += 1
        elif len(defs) == 0:
            problems.append("missing: " + name)
        else:
            problems.append("duplicated (%d): %s" % (len(defs), name))
    print("%s -> %s: %d/%d" % (label, impl, ok, len(names)))
    for p in problems:
        print("  " + p)
    return ok == len(names)


results = [check(CORE + "/mt_matter.h", PORT + "/mt_matter_stub.c", "mt_matter.h"),
           check(CORE + "/mt_devtypes.h", PORT + "/mt_devtypes_stub.c", "mt_devtypes.h"),
           check(CORE + "/hearth_port.h", PORT + "/hearth_port_sl.c", "hearth_port.h")]
sys.exit(0 if all(results) else 1)
