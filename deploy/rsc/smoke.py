#!/usr/bin/env python3
"""Run inside separate client pods using only Python's standard library.

This checks the RSC HTTP API and exact bytes, not Wake execution or BLAKE3.
"""
import json
import os
import sys
import uuid
from urllib.parse import urlsplit
from urllib.request import Request, urlopen


def request(url, data=None, headers=None):
    with urlopen(Request(url, data=data, headers=headers or {}), timeout=60) as response:
        return response.read()


def post_json(url, value, headers=None):
    return request(url, json.dumps(value).encode(), {
        "Content-Type": "application/json", **(headers or {})})


def run(mode, base, run_id):
    base = base.rstrip("/")
    # Large binary payload always uses the filesystem store, never blob/small.
    payload = bytes(range(256)) * 1024 + b"\x00PVC persistence\xff"
    key = {"cmd": list(b"rsc-kubernetes-smoke"), "env": [], "cwd": "/build",
           "stdin": "", "is_atty": False, "hidden_info": run_id, "visible_files": []}
    if mode == "push":
        auth = {"Authorization": os.environ["RSC_API_KEY"]}
        request(base + "/auth/check", b"", auth)
        upload = json.loads(request(base + "/blob"))["url"]
        if urlsplit(upload).scheme not in ("http", "https"):
            raise RuntimeError("upload discovery did not return an HTTP(S) URL")
        boundary = "rsc-smoke-" + uuid.uuid4().hex
        body = (f"--{boundary}\r\nContent-Disposition: form-data; name=\"output\"; "
                f"filename=\"output.bin\"\r\nContent-Type: application/octet-stream\r\n\r\n").encode()
        body += payload + f"\r\n--{boundary}--\r\n".encode()
        # No content_hash field: the API permits it; byte equality is checked below.
        result = json.loads(request(upload, body, {
            **auth, "Content-Type": "multipart/form-data; boundary=" + boundary}))
        assert result["type"] == "Ok", result
        blob_id = result["blobs"][0]["id"]
        post_json(base + "/job", {
            **key, "output_dirs": [], "output_symlinks": [],
            "output_files": [{"path": "output.bin", "mode": 420, "blob_id": blob_id}],
            "stdout_blob_id": blob_id, "stderr_blob_id": blob_id, "status": 0,
            "runtime": 1.0, "cputime": 1.0, "memory": 0, "ibytes": 0,
            "obytes": len(payload), "label": "rsc-kubernetes-smoke:" + run_id,
        }, auth)
        print("PASS: first client uploaded a binary blob and published a job")
    elif mode == "pull":
        result = json.loads(post_json(base + "/job/matching", key))
        assert result["type"] == "Match", result
        assert result["status"] == 0
        blobs = [result["output_files"][0]["blob"], result["stdout_blob"], result["stderr_blob"]]
        for blob in blobs:
            url = blob["url"]
            assert urlsplit(url).scheme in ("http", "https"), url
            assert "/var/lib/rsc" not in url, url
            assert request(url) == payload, "downloaded bytes differ"
        print("PASS: second client matched the job and downloaded exact bytes over HTTP(S)")
    else:
        raise ValueError("choose push or pull")


if __name__ == "__main__":
    if len(sys.argv) != 4:
        sys.exit("usage: smoke.py push|pull BASE_URL RUN_ID")
    run(*sys.argv[1:])
