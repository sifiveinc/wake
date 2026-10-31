#!/usr/bin/env python3
"""Launched by the ignored Rust integration test inside a private mount namespace."""
import concurrent.futures
import contextlib
import http.client
import http.server
import json
import os
from pathlib import Path
import shutil
import stat
import subprocess
import sys
import tempfile
import threading
import urllib.parse
import urllib.request


@contextlib.contextmanager
def client_directory():
    with tempfile.TemporaryDirectory(prefix="rsc-isolated-clients-") as directory:
        try:
            yield directory
        finally:
            # Wake's FUSE helpers may still be shutting down after the last HTTP job.
            # Detach only mounts owned by this smoke in this private namespace.
            points = [line.split()[4] for line in Path("/proc/self/mountinfo").read_text().splitlines()]
            for point in sorted(points, key=len, reverse=True):
                if point.startswith(directory + "/"):
                    result = subprocess.run(["umount", "-l", point], capture_output=True, text=True)
                    remaining = [line.split()[4] for line in Path("/proc/self/mountinfo").read_text().splitlines()]
                    assert result.returncode == 0 or point not in remaining, result.stderr


def main():
    origin, blob_root, wake = sys.argv[1:]
    blob_root = Path(blob_root)
    wake = Path(wake).resolve()
    # This must be the empty tmpfs, not merely a different current directory.
    assert blob_root.is_mount(), "server blob root is not hidden by a mount"
    assert list(blob_root.iterdir()) == [], "client can see server storage"
    try:
        (blob_root / "server-only-sentinel").open("rb")
    except FileNotFoundError:
        pass
    else:
        raise AssertionError("client could open server-only file")
    print("Isolation: private Linux user/mount namespace; empty tmpfs hides server blob root; sentinel open fails.", flush=True)

    state = {"executions": 0, "requests": [], "jobs": [], "matches": [], "fault": None}
    lock = threading.Lock()
    upstream = urllib.parse.urlsplit(origin)

    class Proxy(http.server.BaseHTTPRequestHandler):
        def log_message(self, *_):
            pass

        def do_GET(self):
            self.forward()

        def do_POST(self):
            self.forward()

        def forward(self):
            if self.path == "/executed":
                with lock:
                    state["executions"] += 1
                self.send_response(200)
                self.end_headers()
                return
            data = self.rfile.read(int(self.headers.get("Content-Length", 0)))
            with lock:
                state["requests"].append((self.command, self.path))
                if self.path == "/job" and self.command == "POST":
                    state["jobs"].append(json.loads(data))
            conn = http.client.HTTPConnection(upstream.hostname, upstream.port, timeout=30)
            headers = {k: v for k, v in self.headers.items() if k.lower() not in ("host", "connection")}
            conn.request(self.command, self.path, data, headers)
            response = conn.getresponse()
            body = response.read()
            status = response.status
            content_type = response.getheader("Content-Type", "application/octet-stream")
            conn.close()
            if self.path == "/job/matching" and status == 200:
                match = json.loads(body)
                if match.get("type") == "Match":
                    with lock:
                        state["matches"].append(match)
                # Force the real Wake client's downloads through this observable proxy.
                body = body.replace(origin.encode(), proxy_url.encode())
            fault = state["fault"]
            if fault and self.path == fault[1] and self.command == "GET":
                if fault[0] == "missing":
                    status, body = 404, b""
                elif fault[0] == "corrupt":
                    body = bytes([body[0] ^ 255]) + body[1:]
            self.send_response(status)
            self.send_header("Content-Type", content_type)
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

    proxy = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Proxy)
    proxy_url = f"http://127.0.0.1:{proxy.server_port}"
    thread = threading.Thread(target=proxy.serve_forever, daemon=True)
    thread.start()
    fixture = Path(__file__).with_name("test.wake")
    hasher = wake.parent.parent / "lib/wake/wake-hash"
    assert hasher.is_file(), f"missing Wake helper {hasher}"
    env = dict(os.environ, DEBUG_WAKE_SHARED_CACHE="1", RSC_TEST_PORT=str(proxy.server_port),
               RSC_TEST_MARKER=proxy_url + "/executed", RSC_TEST_KEY="portable-primary", RSC_TEST_POLICY="pushpull")
    # Do not accidentally use a caller's global cache configuration or credentials.
    for key in ("WAKE_SHARED_CACHE", "WAKE_REMOTE_CACHE"):
        env.pop(key, None)
    expected = {"result/small.bin": bytes(range(256)) + b"\0",
                "result/large.bin": bytes(range(256)) * 513, "result/empty": b""}

    with client_directory() as directory:
        root = Path(directory)

        def run(name, policy="pushpull", key="portable-primary", target="portable Unit", extra=None):
            workspace = root / name
            workspace.mkdir()
            shutil.copy(fixture, workspace / "test.wake")
            (workspace / ".wakeroot").write_text("{}\n")
            # Each client starts with neither a Wake database nor any local CAS.
            assert not (workspace / "wake.db").exists()
            assert not (workspace / ".build").exists()
            child_env = dict(env, RSC_TEST_POLICY=policy, RSC_TEST_KEY=key, **(extra or {}))
            for args in (["--init", "."], ["--in", "remote_cache", "-x", target]):
                result = subprocess.run([str(wake), *args], cwd=workspace, env=child_env,
                                        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=90)
                assert result.returncode == 0, f"{name}: Wake {args} failed:\n{result.stdout}"
                assert "Aborting" not in result.stdout and "Fail " not in result.stdout, f"{name}: {result.stdout}"
                if "-x" in args:
                    assert "Pass Unit" in result.stdout, f"{name}: no successful test result:\n{result.stdout}"
            if target == "portable Unit":
                for path, contents in expected.items():
                    assert (workspace / path).read_bytes() == contents, f"{name}: bytes differ: {path}"
                assert stat.S_IMODE((workspace / "result/small.bin").stat().st_mode) == 0o751
                assert stat.S_IMODE((workspace / "result/large.bin").stat().st_mode) == 0o640
                assert stat.S_IMODE((workspace / "result/empty-dir").stat().st_mode) == 0o750
                assert (workspace / "result/empty-dir").is_dir()
                assert os.readlink(workspace / "result/link") == "small.bin"
                assert (workspace / "result/link").read_bytes() == expected["result/small.bin"]
            return workspace

        first = run("producer")
        assert state["executions"] == 1, "producer did not execute exactly once"
        assert len(state["jobs"]) == 1, "producer failed to upload the completed job"
        posted = state["jobs"][0]
        assert posted["status"] == 0
        assert posted["output_dirs"] and posted["output_symlinks"]
        before = len(state["requests"])
        second = run("fresh-consumer", policy="pull")
        assert state["executions"] == 1, "fresh consumer executed instead of restoring"
        assert len(state["jobs"]) == 1, "pull-only consumer uploaded a job"
        assert any(method == "GET" and path.startswith("/blob/") for method, path in state["requests"][before:])
        matched = state["matches"][-1]
        assert matched["stdout_blob"]["url"].startswith(origin + "/blob/")
        assert matched["stderr_blob"]["url"].startswith("db://")
        files = {f["path"]: f for f in matched["output_files"]}
        assert files["result/empty"]["blob"]["url"] == "db://"
        for path in expected:
            # Wake's own BLAKE3 helper independently checks all restored file hashes.
            actual_hash = subprocess.check_output([str(hasher), str(second / path)], text=True).strip()
            assert files[path]["blob"]["content_hash"] == actual_hash, f"hash differs: {path}"
            assert subprocess.check_output([str(hasher), str(first / path)], text=True).strip() == actual_hash
        binary_url = files["result/small.bin"]["blob"]["url"]
        assert urllib.request.urlopen(binary_url, timeout=10).read() == expected["result/small.bin"]
        print("PASS: actual Wake upload and fresh pull-only restoration; binary bytes, BLAKE3 hashes, modes, directories, symlinks, stdout/stderr and status.", flush=True)

        # Separate fresh clients restore concurrently, without a shared local database or CAS.
        with concurrent.futures.ThreadPoolExecutor(max_workers=3) as pool:
            list(pool.map(lambda i: run(f"concurrent-{i}", policy="pull"), range(3)))
        assert state["executions"] == 1, "concurrent consumers missed the cached result"
        print("PASS: concurrent fresh Wake clients restore over HTTP.", flush=True)

        before = len(state["requests"])
        run("push-only", policy="push")
        assert state["executions"] == 2, "push-only unexpectedly restored from cache"
        assert ("POST", "/job/matching") not in state["requests"][before:]
        before_jobs = len(state["jobs"])
        before = len(state["requests"])
        run("pull-only-miss", policy="pull", key="uncached-key")
        assert state["executions"] == 3
        assert len(state["jobs"]) == before_jobs
        assert ("POST", "/blob") not in state["requests"][before:]
        print("PASS: push-only skips matching; pull-only miss executes without uploads.", flush=True)

        run("hash-rejection", target="badHash Unit", extra={"RSC_TEST_BLOB_URL": binary_url})
        path = urllib.parse.urlsplit(binary_url).path
        for fault in ("corrupt", "missing"):
            state["fault"] = (fault, path)
            before_executions = state["executions"]
            workspace = run(f"fallback-{fault}", policy="pull")
            assert state["executions"] == before_executions + 1, f"{fault} did not fall back to local execution"
            assert not (workspace / "must-not-exist").exists()
            state["fault"] = None
        print("PASS: incorrect hash cannot materialize a file; corrupt/missing HTTP blob falls back safely to local execution.", flush=True)
    proxy.shutdown()
    proxy.server_close()


if __name__ == "__main__":
    main()
