#!/usr/bin/env python3
"""Build the version-1 single-file image (last sector reserved for DMA tests)."""
import pathlib
import struct
import sys

source, target = map(pathlib.Path, sys.argv[1:])
data = source.read_bytes()
name = source.name.encode()
if len(name) > 23 or not name or len(data) > 1048576 - 1024:
    raise SystemExit("file does not fit tinyfs")
image = bytearray(1048576)
struct.pack_into('<QQ24s', image, 0, 0x315346594e4954, len(data), name)
image[512:512 + len(data)] = data
target.parent.mkdir(parents=True, exist_ok=True)
target.write_bytes(image)
