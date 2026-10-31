#!/usr/bin/env python3
"""Resolve and download the Debian dependency closure of a package.

Reads the bookworm main/binary-amd64 Packages index (already downloaded as
build/debs/Packages.xz), walks Depends/Pre-Depends, and downloads every
concrete package into build/debs/.  Alternatives ("a | b") take the first
candidate that exists in the index; virtual packages (no Package entry) are
skipped, which is fine for the runtime libraries we need.

usage:
    fetch_deb_closure.py chromium --max-packages 400
    fetch_deb_closure.py libglib2.0-bin --list-only
"""
from __future__ import annotations

import argparse
import lzma
import re
import subprocess
import sys
import urllib.request
from pathlib import Path

DEBS = Path(__file__).resolve().parent.parent / "build/debs"
INDEX = DEBS / "Packages.xz"
BASE = "http://deb.debian.org/debian/"


def load_index() -> dict[str, dict]:
    with lzma.open(INDEX, "rt", encoding="utf-8", errors="replace") as f:
        data = f.read()
    packages: dict[str, dict] = {}
    for block in data.split("\n\n"):
        if not block.strip():
            continue
        fields: dict[str, str] = {}
        key = None
        for line in block.splitlines():
            if line.startswith(" ") and key:
                fields[key] += " " + line.strip()
                continue
            if ": " in line:
                key, value = line.split(": ", 1)
                fields[key] = value
        name = fields.get("Package")
        if name and name not in packages:
            packages[name] = fields
    return packages


def dep_names(fields: dict) -> list[str]:
    out: list[str] = []
    for key in ("Pre-Depends", "Depends"):
        for item in fields.get(key, "").split(","):
            item = item.strip()
            if not item:
                continue
            first = item.split("|")[0].strip()
            first = re.sub(r"\s*[(:].*$", "", first).strip()
            if first:
                out.append(first)
    return out


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("root", nargs="+")
    ap.add_argument("--max-packages", type=int, default=400)
    ap.add_argument("--list-only", action="store_true")
    ap.add_argument("--skip", action="append", default=[],
                    help="package to leave out of the closure")
    args = ap.parse_args()

    if not INDEX.is_file():
        print(f"missing index {INDEX}: download Packages.xz first", file=sys.stderr)
        return 2
    index = load_index()

    order: list[str] = []
    seen: set[str] = set()
    queue = list(args.root)
    while queue:
        name = queue.pop(0)
        if name in seen or name in args.skip:
            continue
        seen.add(name)
        fields = index.get(name)
        if not fields:
            print(f"[skip] {name}: not a concrete package", file=sys.stderr)
            continue
        order.append(name)
        if len(order) > args.max_packages:
            print(f"closure exceeds --max-packages {args.max_packages}",
                  file=sys.stderr)
            return 3
        queue.extend(dep_names(fields))

    total = 0
    for name in order:
        fields = index[name]
        size = int(fields.get("Size", "0"))
        total += size
        filename = fields["Filename"]
        target = DEBS / Path(filename).name
        print(f"{name:32s} {size/1024:9.0f} KiB  {filename}")
        if args.list_only or target.is_file():
            continue
        url = BASE + filename
        try:
            urllib.request.urlretrieve(url, target)
        except Exception as exc:                 # noqa: BLE001
            print(f"  download failed: {exc}", file=sys.stderr)
            return 4
    print(f"\n{len(order)} packages, {total/1024/1024:.1f} MiB of .deb payload")
    return 0


if __name__ == "__main__":
    sys.exit(main())
