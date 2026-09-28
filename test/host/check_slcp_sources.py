#!/usr/bin/env python3
"""check_slcp_sources.py - the core/ source list in platform/silabs/hearth.slcp
must equal core/sources.cmake's HEARTH_CORE_SOURCES. A path added to one and
not the other is a build that is green against a list that is no longer the
list of record (platform/silabs/README.md, "Building")."""
import re
import sys

CMAKE = "../../core/sources.cmake"
SLCP = "../../platform/silabs/hearth.slcp"

# sources.cmake writes each entry as ${HEARTH_CORE_DIR}/<sub>/<file>.c inside
# one set(HEARTH_CORE_SOURCES ...). Strip the variable, keep the rest.
cm = open(CMAKE).read()
block = re.search(r'set\(HEARTH_CORE_SOURCES(.*?)\)', cm, re.S).group(1)
cmake_paths = sorted("core/" + p.split('}', 1)[-1].lstrip('/')
                     for p in block.split() if p.endswith('.c'))

# The .slcp writes the same files relative to platform/silabs/, under its
# source: key. Only that block is read: the include: key names ../../core
# too (the headers), and those are not sources.
slcp = open(SLCP).read()
source_block = re.search(r'^source:\n(.*?)(?=^\S)', slcp, re.S | re.M).group(1)
slcp_paths = sorted("core/" + m
                    for m in re.findall(r'path:\s*\.\./\.\./core/(\S+)', source_block))

if cmake_paths != slcp_paths:
    print("core source lists differ")
    print("  sources.cmake:", cmake_paths)
    print("  hearth.slcp:  ", slcp_paths)
    sys.exit(1)
print("hearth.slcp core sources == core/sources.cmake (%d files)" % len(slcp_paths))
