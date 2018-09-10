#!/usr/bin/env python

import hashlib
import os
import sys

DEFINES = ["MAJORVER", "MINORVER", "REVISION", "PATCHLVL"]
LEVELS = ["major", "minor", "rev", "patch"]

# TODO: Get from sys.argv
bump = None

hash = hashlib.sha256()

for subdir in ("src", "res"):
    for root, dirs, files in os.walk(subdir):
        for name in files:
            if subdir == "src" and name == "bbl_version.h":
                continue

            with open(os.path.join(root, name), "rb") as fp:
                for chunk in iter(lambda: fp.read(4096), b""):
                    hash.update(chunk)

new_source_hash = '"%s"' % hash.hexdigest()

lines = []
with open("src/bbl_version.h") as fp:
    for line in fp:
        for i in range(len(DEFINES)):
            prefix = "#define %s" % DEFINES[i]
            if line.startswith(prefix) and bump in LEVELS[0:i+1]:
                if bump in LEVELS[0:i]:
                    line = "%s 0\n" % prefix
                else:
                    line = "%s %d\n" % (prefix, (int(line[len(prefix):]) + 1))
                break
        else:
            if line.startswith("#define BBL_SOURCE_HASH"):
                old_source_hash = line[len("#define BBL_SOURCE_HASH"):].strip()
                line = "#define BBL_SOURCE_HASH %s\n" % new_source_hash

        lines.append(line)

if new_source_hash != old_source_hash:
    with open("src/bbl_version.h", "w") as fp:
        fp.writelines(lines)
