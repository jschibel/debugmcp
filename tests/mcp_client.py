"""Minimal MCP stdio client used by the test-suite (and handy for manual poking).

    python tests/mcp_client.py launch '{"program": "...", "stop_at": "main"}' locals '{}'
"""
import json
import os
import subprocess
import sys
import threading
import queue

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.path.join(ROOT, "bin", "debugmcp.exe")


class McpError(Exception):
    pass


def _run_cmd(script):
    print(f"[setup] running {os.path.relpath(script, ROOT)} ...", flush=True)
    r = subprocess.run(["cmd.exe", "/d", "/c", script], cwd=os.path.dirname(script))
    if r.returncode != 0:
        raise SystemExit(f"{script} failed with exit code {r.returncode}")


def _stale(outputs, inputs):
    if not all(os.path.exists(o) for o in outputs):
        return True
    newest_in = max(os.path.getmtime(i) for i in inputs)
    return min(os.path.getmtime(o) for o in outputs) < newest_in


def ensure_server_built():
    """Builds bin/debugmcp.exe (and fetches the DbgEng runtime) if missing or older than src/."""
    src = os.path.join(ROOT, "src")
    inputs = [os.path.join(src, f) for f in os.listdir(src)]
    if _stale([EXE], inputs):
        _run_cmd(os.path.join(ROOT, "build.cmd"))


def ensure_targets_built():
    """Builds the C++ test targets if missing or older than their sources."""
    tdir = os.path.join(ROOT, "tests", "targets")
    tbin = os.path.join(tdir, "bin")
    outputs = [os.path.join(tbin, n) for n in ("basic.exe", "basic_opt.exe", "threads.exe", "plugin.dll")]
    inputs = [os.path.join(tdir, n) for n in os.listdir(tdir) if n.endswith((".cpp", ".cmd"))]
    if _stale(outputs, inputs):
        _run_cmd(os.path.join(tdir, "build.cmd"))


class Client:
    def __init__(self, exe=EXE):
        self.p = subprocess.Popen([exe], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=subprocess.PIPE, bufsize=0)
        self._id = 0
        self._responses = {}
        self._cv = threading.Condition()
        self.stderr_lines = []
        threading.Thread(target=self._reader, daemon=True).start()
        threading.Thread(target=self._err_reader, daemon=True).start()
        r = self.request("initialize", {"protocolVersion": "2025-06-18", "capabilities": {},
                                        "clientInfo": {"name": "test", "version": "0"}})
        self.server_info = r
        self.notify("notifications/initialized")

    def _reader(self):
        for line in self.p.stdout:
            line = line.strip()
            if not line:
                continue
            msg = json.loads(line)
            with self._cv:
                self._responses[msg.get("id")] = msg
                self._cv.notify_all()
        with self._cv:
            self._responses["__eof__"] = True
            self._cv.notify_all()

    def _err_reader(self):
        for line in self.p.stderr:
            self.stderr_lines.append(line.decode(errors="replace").rstrip())

    def notify(self, method, params=None):
        msg = {"jsonrpc": "2.0", "method": method}
        if params is not None:
            msg["params"] = params
        self.p.stdin.write((json.dumps(msg) + "\n").encode())
        self.p.stdin.flush()

    def send_request(self, method, params=None):
        self._id += 1
        rid = self._id
        msg = {"jsonrpc": "2.0", "id": rid, "method": method, "params": params or {}}
        self.p.stdin.write((json.dumps(msg) + "\n").encode())
        self.p.stdin.flush()
        return rid

    def wait_response(self, rid, timeout=120):
        with self._cv:
            ok = self._cv.wait_for(lambda: rid in self._responses or "__eof__" in self._responses, timeout)
            if not ok:
                raise TimeoutError(f"no response for request {rid}")
            if rid not in self._responses:
                raise McpError("server exited: " + "\n".join(self.stderr_lines[-20:]))
            msg = self._responses.pop(rid)
        if "error" in msg:
            raise McpError(msg["error"])
        return msg["result"]

    def request(self, method, params=None, timeout=120):
        return self.wait_response(self.send_request(method, params), timeout)

    def call_raw(self, tool, args=None, timeout=120):
        r = self.request("tools/call", {"name": tool, "arguments": args or {}}, timeout)
        text = r["content"][0]["text"]
        return r.get("isError", False), text

    def call(self, tool, args=None, timeout=120, expect_error=False):
        is_err, text = self.call_raw(tool, args, timeout)
        if is_err and not expect_error:
            raise McpError(f"{tool} failed: {text}")
        if expect_error and not is_err:
            raise McpError(f"{tool} unexpectedly succeeded: {text}")
        if is_err:
            return text
        try:
            return json.loads(text)
        except json.JSONDecodeError:
            return text

    def call_async(self, tool, args=None):
        return self.send_request("tools/call", {"name": tool, "arguments": args or {}})

    def result_of(self, rid, timeout=120):
        r = self.wait_response(rid, timeout)
        return json.loads(r["content"][0]["text"])

    def close(self):
        try:
            self.p.stdin.close()
            self.p.wait(timeout=20)
        except Exception:
            self.p.kill()


if __name__ == "__main__":
    c = Client()
    args = sys.argv[1:]
    try:
        for i in range(0, len(args), 2):
            tool = args[i]
            a = json.loads(args[i + 1]) if i + 1 < len(args) else {}
            is_err, text = c.call_raw(tool, a)
            print(f"=== {tool} {'(ERROR)' if is_err else ''}")
            print(text)
    finally:
        c.close()
        if c.stderr_lines:
            print("--- stderr ---")
            print("\n".join(c.stderr_lines[-30:]))
