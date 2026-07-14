#!/usr/bin/env python3

import json
import os
import sys


def main() -> int:
    # fuse-waked only exposes paths listed in the job's "visible" JSON plus
    # files created by that same job. On remount, a fresh job starts, so we
    # pre-seed visibility with the existing backing tree.
    if len(sys.argv) != 2:
        print("usage: wakefs_visible.py <root>", file=sys.stderr)
        return 1

    root = sys.argv[1]
    visible = []

    # Emit both directories and files as relative paths. Sorting keeps the JSON
    # stable, which makes debugging and reproducing test behavior easier.
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames.sort()
        filenames.sort()

        rel_dir = os.path.relpath(dirpath, root)
        if rel_dir != ".":
            visible.append(rel_dir)

        for name in filenames:
            rel = os.path.join(rel_dir, name) if rel_dir != "." else name
            visible.append(rel)

    # The helper writes this JSON to .i.<jobid> for fuse-waked to parse.
    json.dump({"visible": visible}, sys.stdout, separators=(",", ":"))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
