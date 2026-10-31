#!/usr/bin/env python3
"""Render example YAML to stdout without reading or emitting credentials."""
import json
import os
from pathlib import Path
import string
import sys
import uuid
from urllib.parse import urlsplit


def render(name):
    if name not in ("rsc", "admin", "clients"):
        raise ValueError("choose rsc, admin, or clients")
    template = string.Template(Path(__file__).with_name(name + ".yaml").read_text())
    values = {
        "RSC_SECRET": "rsc-credentials",
        "RSC_PVC": "rsc-cache",
        "RSC_STORAGE_CAPACITY": "10Gi",
        "RSC_PUBLIC_BASE_URL": "http://rsc:3002",
        "RSC_CLIENT_IMAGE": "python:3.12-slim",
    }
    keys = {match.group("named") or match.group("braced")
            for match in template.pattern.finditer(template.template)}
    keys.discard(None)
    values.update({key: os.environ[key] for key in keys if key in os.environ})
    missing = keys - values.keys()
    if missing:
        raise ValueError("set environment variables: " + ", ".join(sorted(missing)))
    if name == "rsc":
        uuid.UUID(values["RSC_ACTIVE_STORE"])
        public_url = values["RSC_PUBLIC_BASE_URL"]
        url = urlsplit(public_url)
        if (url.scheme not in ("http", "https") or not url.hostname
                or url.username is not None or url.password is not None
                or "?" in public_url or "#" in public_url):
            raise ValueError("RSC_PUBLIC_BASE_URL must be an absolute HTTP(S) URL without credentials, query, or fragment")
    return template.substitute({key: json.dumps(values[key]) for key in keys})


if __name__ == "__main__":
    try:
        if len(sys.argv) != 2:
            raise ValueError("usage: render.py rsc|admin|clients")
        print(render(sys.argv[1]), end="")
    except (ValueError, OSError) as error:
        sys.exit(str(error))
