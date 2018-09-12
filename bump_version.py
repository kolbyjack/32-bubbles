#!/usr/bin/env python

import hashlib
import os
import sys

VERSION_DEFINES = ["MAJORVER", "MINORVER", "REVISION", "PATCHLVL"]
VERSION_LEVELS  = ["major", "minor", "rev", "patch"]

# TODO: Get from sys.argv
bump = None

def update_version(file):
    lines = []

    with open(file, "r") as fp:
        for line in fp:
            for i in range(len(VERSION_DEFINES)):
                prefix = "#define %s" % VERSION_DEFINES[i]
                if line.startswith(prefix) and bump in VERSION_LEVELS[0:i+1]:
                    if bump in VERSION_LEVELS[0:i]:
                        line = "%s 0\n" % prefix
                    else:
                        line = "%s %d\n" % (prefix, (int(line[len(prefix):]) + 1))
                    break

            lines.append(line)

    with open(file, "w") as fp:
        fp.writelines(lines)

def hash_files(dir, skip=[]):
    hash = hashlib.sha1()

    for root, dirs, files in os.walk(dir):
        for name in files:
            if name in skip:
                continue

            with open(os.path.join(root, name), "rb") as fp:
                for chunk in iter(lambda: fp.read(32768), b""):
                    hash.update(chunk)

    return '"%s"' % hash.hexdigest()

def update_hash(file, tag, hash):
    prefix = "#define %s" % tag
    lines = []

    with open(file, "r") as fp:
        for line in fp:
            if line.startswith(prefix):
                old_hash = line[len(prefix):].strip()
                line = "%s %s\n" % (prefix, hash)

            lines.append(line)

    if old_hash != hash:
        with open(file, "w") as fp:
            fp.writelines(lines)

if bump is not None:
    update_version("src/bbl_version.h")
else:
    src_hash = hash_files("src", ["bbl_version.h", "bbl_httpd_resources.h"])
    update_hash("src/bbl_version.h", "BBL_SOURCE_HASH", src_hash)

    httpd_res_hash = hash_files("res")
    update_hash("src/bbl_httpd_resources.h", "BBL_HTTPD_RESOURCES_HASH", httpd_res_hash)
