#!/usr/bin/env python3
"""check_decls.py - every hearth_port.h / mt_matter.h / mt_devtypes.h /
hearth_console.h declaration must have exactly one function definition in
the Silabs port file behind it, or across the set of files that share the
header (stub + sl.cpp, plus hearth_ota_sl.cpp and the shared
hearth_ota_requestor.cpp for mt_matter.h). -fsyntax-only alone proves a stub
compiles clean; it says nothing about a missing or duplicated definition.
This does."""
import os
import re
import sys

CORE = "../../core/include"
PORT = "../../platform/silabs/port"
COMMON = "../../platform/common"
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


INC = re.compile(r'^\s*#include\s+"([^"/]+\.inc)"\s*$')


def read_expanded(path, seen=None):
    """A port file's lines with its local fragment includes inlined, as the
    compiler sees them: #include "name.inc" (same directory, no path) is
    replaced by that fragment's own expanded lines. Since 2026-09-25 the
    MG24 port splits mt_devtypes_sl.cpp and mt_matter_sl.cpp into such
    fragments (graph T549, DE550), and a definition inside one must still
    count exactly once."""
    seen = set() if seen is None else seen
    real = os.path.realpath(path)
    if real in seen:
        return []
    seen.add(real)
    out = []
    for line in open(path).read().splitlines():
        m = INC.match(line)
        if m:
            frag = os.path.join(os.path.dirname(path), m.group(1))
            if os.path.exists(frag):
                out += read_expanded(frag, seen)
                continue
        out.append(line)
    return out


def check(header, impls, label):
    """Exactly one definition per declared name across the CONCATENATION of
    impls, not once per file: as the Matter port lands, a name moves from
    mt_matter_stub.c to mt_matter_sl.cpp and must be defined in exactly one
    of the two, never in both. A listed file that does not exist yet is
    skipped, so this reads the same before and after that file appears.
    Fragments a file #includes are read as part of it."""
    names = names_from(header)
    present = [f for f in impls if os.path.exists(f)]
    lines = []
    for f in present:
        lines += read_expanded(f)
    ok, problems = 0, []
    for name in names:
        pat = re.compile(r'^[a-zA-Z_].*\b' + re.escape(name) + r'\s*\(')
        defs = []
        for line in lines:
            if not pat.match(line):
                continue
            # A trailing comment is not part of the statement, so it is cut
            # before the semicolon test: "void f(void);  /* why */" is a
            # declaration just as much as "void f(void);" is. The brace test
            # is what keeps that cut safe: a one-line definition whose body
            # holds a comment ("void f(int a) { g(a); /* why */ }") also cuts
            # down to text ending in ';', and skipping it would hide a second
            # definition, which is the case this gate exists to catch.
            code = re.sub(r'(/\*.*|//.*)$', '', line).rstrip()
            if code.endswith(';') and '{' not in code:
                continue                    # a declaration, not a definition
            if '//' in line.split(name, 1)[0]:
                continue                    # the name sits in a C++ comment
            defs.append(line)
        if len(defs) == 1:
            ok += 1
        elif len(defs) == 0:
            problems.append("missing: " + name)
        else:
            problems.append("duplicated (%d): %s" % (len(defs), name))
    print("%s -> %s: %d/%d" % (label, ", ".join(present) or "(no impl file)",
                               ok, len(names)))
    for p in problems:
        print("  " + p)
    return 0 if ok == len(names) else 1


rc = 0
rc |= check(CORE + "/mt_matter.h",
            [PORT + "/mt_matter_stub.c", PORT + "/mt_matter_sl.cpp",
             PORT + "/hearth_ota_sl.cpp", COMMON + "/hearth_ota_requestor.cpp"],
            "mt_matter.h")
rc |= check(CORE + "/mt_devtypes.h",
            [PORT + "/mt_devtypes_stub.c", PORT + "/mt_devtypes_sl.cpp"], "mt_devtypes.h")
rc |= check(CORE + "/hearth_port.h", [PORT + "/hearth_port_sl.c"], "hearth_port.h")
rc |= check(PORT + "/hearth_console.h", [PORT + "/hearth_port_sl.c"], "hearth_console.h")
sys.exit(rc)
