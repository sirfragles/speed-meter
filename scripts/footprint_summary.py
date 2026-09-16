#!/usr/bin/env python3
#
# Copyright (c) 2026 Mateusz Żerebecki
#
# SPDX-License-Identifier: Apache-2.0
#
# Render the ROM/RAM report from `west build -t footprint` as Markdown.
#
# `west build -t footprint` runs scripts/footprint/size_report, which writes
# ram.json and rom.json into the build directory. Those files group every
# symbol by the source path it came from, so they answer the question the
# number alone cannot: *which part of the build* is using the flash.
#
# This script turns them into a table short enough for a GitHub job summary
# (append it to $GITHUB_STEP_SUMMARY) and readable in a terminal.
#
# Usage:
#   scripts/footprint_summary.py build
#   scripts/footprint_summary.py build --title production --top 8
#
# It exits non-zero only when a report is missing or malformed, never because a
# size looks large: a size threshold that lives in CI gets raised instead of
# investigated, and the real budget is the linker's.

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

# size_report parks symbols it could not attribute to a source path here. They
# say nothing about where the size goes, so they are left out.
_NOISE = {"(hidden)", "(no paths)"}

# Top-level buckets that carry a fixed meaning in size_report's tree: it
# substitutes these two names for the paths it was given on the command line.
_ZEPHYR = "ZEPHYR_BASE"
_GENERATED = "OUTPUT_DIR"

_LABELS = {
    "/": "application / other",
}


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Render a Zephyr footprint report as Markdown.")
    parser.add_argument("build_dir", type=Path,
                        help="build directory holding ram.json and rom.json")
    parser.add_argument("--title", default=None,
                        help="heading text (default: the directory name)")
    parser.add_argument("--top", type=int, default=6,
                        help="groups listed per region (default: %(default)s)")
    return parser.parse_args()


def shorten(name: str) -> str:
    """Keep the last two path components so host paths do not leak in."""
    parts = [part for part in name.split("/") if part not in ("", ".")]
    if len(parts) > 1:
        return "/".join(parts[-2:])
    return name


def collect(node: dict) -> list[tuple[str, int]]:
    """Flatten the report tree into the (label, bytes) pairs worth showing.

    ZEPHYR_BASE is expanded one level on purpose: reporting a single
    "zephyr" entry would hide exactly the subsystems worth watching
    (kernel, subsys/bluetooth, drivers, ...).
    """
    groups: list[tuple[str, int]] = []

    for child in node.get("children", []):
        name = child["name"]
        size = child["size"]

        if name == _ZEPHYR:
            for sub in child.get("children", []):
                if sub["name"] not in _NOISE:
                    groups.append((f"zephyr/{shorten(sub['name'])}", sub["size"]))
        elif name == _GENERATED:
            if size:
                groups.append(("generated", size))
        elif name in _NOISE:
            continue
        else:
            groups.append((_LABELS.get(name, shorten(name)), size))

    return groups


def kib(size: int) -> str:
    return f"{size / 1024:.1f} KiB"


def render_region(title: str, path: Path, top: int) -> list[str]:
    with path.open(encoding="utf-8") as handle:
        report = json.load(handle)

    total = report["total_size"]
    groups = sorted(collect(report["symbols"]), key=lambda item: -item[1])
    shown = groups[:top]
    rest = groups[top:]

    lines = [f"**{title}** — {kib(total)} total", ""]
    lines.append("| Group | Size | Share |")
    lines.append("| --- | ---: | ---: |")

    for label, size in shown:
        share = (100.0 * size / total) if total else 0.0
        lines.append(f"| {label} | {kib(size)} | {share:.1f}% |")

    if rest:
        remainder = sum(size for _, size in rest)
        share = (100.0 * remainder / total) if total else 0.0
        lines.append(
            f"| *{len(rest)} more* | {kib(remainder)} | {share:.1f}% |")

    lines.append("")
    return lines


def main() -> int:
    args = parse_args()
    title = args.title or args.build_dir.name

    lines = [f"### Footprint — {title}", ""]
    found = False

    for region, filename in (("ROM", "rom.json"), ("RAM", "ram.json")):
        path = args.build_dir / filename
        if not path.is_file():
            print(f"footprint_summary: no {path}", file=sys.stderr)
            continue
        found = True
        lines.extend(render_region(region, path, args.top))

    if not found:
        print("footprint_summary: run `west build -t footprint` first",
              file=sys.stderr)
        return 1

    print("\n".join(lines))
    return 0


if __name__ == "__main__":
    sys.exit(main())
