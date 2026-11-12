#!/usr/bin/env python3
"""End to end: a headless Shady with a foot terminal, driven over MCP.

foot runs a shell that reads one line into a file, so what `type` and `key`
send must really reach the window. Screenshots come back as PNG images, and
clicks, moves and drags land at the layout point of the image pixel given.
"""
import argparse
import base64
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parent.parent


class Fail(Exception):
    pass


def check(cond, message):
    if not cond:
        raise Fail(message)


class Mcp:
    def __init__(self, argv, env):
        self.proc = subprocess.Popen(argv, env=env, stdin=subprocess.PIPE,
                                     stdout=subprocess.PIPE, text=True)
        self.next_id = 1

    def request(self, method, params=None):
        rid = self.next_id
        self.next_id += 1
        msg = {"jsonrpc": "2.0", "id": rid, "method": method}
        if params is not None:
            msg["params"] = params
        self.proc.stdin.write(json.dumps(msg) + "\n")
        self.proc.stdin.flush()
        line = self.proc.stdout.readline()
        check(line, f"{method}: shady-mcp closed stdout")
        reply = json.loads(line)
        check(reply.get("id") == rid, f"{method}: answer for {reply.get('id')}")
        return reply

    def notify(self, method):
        self.proc.stdin.write(json.dumps({"jsonrpc": "2.0", "method": method}) + "\n")
        self.proc.stdin.flush()

    def tool(self, name, ok=True, **args):
        reply = self.request("tools/call", {"name": name, "arguments": args})
        check("result" in reply, f"{name}: {reply}")
        result = reply["result"]
        check(result["isError"] != ok, f"{name} {args}: {result['content']}")
        return result["content"]

    def close(self):
        self.proc.stdin.close()
        return self.proc.wait(timeout=10)


def text_of(content):
    return "\n".join(c["text"] for c in content if c["type"] == "text")


def wait_for(pred, what, timeout=10.0):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        if pred():
            return
        time.sleep(0.05)
    raise Fail(f"timed out waiting for {what}")


def ipc(sock, cmd, **args):
    import socket
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.connect(sock)
    s.sendall((json.dumps({"cmd": cmd, **args}) + "\n").encode())
    data = b""
    while not data.endswith(b"\n"):
        data += s.recv(65536)
    s.close()
    return json.loads(data)


def scenario(mcp, sock, out_file):
    init = mcp.request("initialize", {"protocolVersion": "2025-06-18", "capabilities": {},
                                      "clientInfo": {"name": "test", "version": "0"}})
    check(init["result"]["protocolVersion"] == "2025-06-18", f"initialize {init}")
    check(init["result"]["capabilities"] == {"tools": {}}, "capabilities")
    mcp.notify("notifications/initialized")
    names = {t["name"] for t in mcp.request("tools/list")["result"]["tools"]}
    check({"screenshot", "click", "move", "drag", "scroll", "type", "key", "wait",
           "windows", "focus_window", "shady_ipc"} <= names, f"tools {names}")
    check(mcp.request("ping")["result"] == {}, "ping")
    check(mcp.request("no/such")["error"]["code"] == -32601, "unknown method")

    windows = text_of(mcp.tool("windows"))
    check('"foot"' in windows and "focused" in windows, f"windows: {windows}")

    # A screenshot, half size: an image, and its geometry.
    out = ipc(sock, "outputs")["data"][0]
    ow, oh = out["width"], out["height"]
    shot = mcp.tool("screenshot", max_width=ow // 2)
    images = [c for c in shot if c["type"] == "image"]
    check(len(images) == 1 and images[0]["mimeType"] == "image/png", "screenshot image")
    png = base64.b64decode(images[0]["data"])
    check(png[:8] == b"\x89PNG\r\n\x1a\n", "not a PNG")
    check(struct.unpack(">II", png[16:24]) == (ow // 2, oh // 2), "image size")
    check(f"{ow // 2}x{oh // 2} screenshot" in text_of(shot), text_of(shot))

    # Image pixels are layout points times two now.
    mcp.tool("move", x=100, y=50)
    pointer = ipc(sock, "pointer")["data"]
    check((pointer["x"], pointer["y"]) == (200, 100), f"move mapped to {pointer}")
    mcp.tool("drag", x1=150, y1=150, x2=200, y2=180, duration=150)
    pointer = ipc(sock, "pointer")["data"]
    check((pointer["x"], pointer["y"]) == (400, 360), f"drag ended at {pointer}")
    mcp.tool("scroll", x=ow // 4, y=oh // 4, dy=1)

    # Click into the terminal, then type a line, Hangul included, and Return.
    foot = next(w for w in ipc(sock, "windows")["data"] if w["app_id"] == "foot")
    cx = (foot["x"] + foot["width"] / 2) / 2
    cy = (foot["y"] + foot["height"] / 2) / 2
    text = text_of(mcp.tool("click", x=cx, y=cy))
    check("clicked" in text, text)
    line = "hello from shady-mcp, 안녕 ✓"
    check(f"typed {len(line)} characters" in text_of(mcp.tool("type", text=line)), "type")
    check("sent to the focused window" in text_of(mcp.tool("key", keys="Return")), "Return")
    wait_for(lambda: out_file.exists() and out_file.read_text() == line, "the line in foot")
    check("settled" in text_of(mcp.tool("wait", idle=100)), "wait")
    check("settled after" in text_of(mcp.tool("wait", idle=100, window=foot["id"])), "wait on a window")
    # idle longer than timeout: it cannot settle, and says so.
    check("still changing" in text_of(mcp.tool("wait", scope="screen", idle=5000, timeout=300)),
          "a wait that cannot settle in time")

    # A window screenshot maps from the window's corner.
    shot = mcp.tool("screenshot", window=foot["id"])
    check(f"{foot['width']}x{foot['height']} screenshot of a window" in text_of(shot), text_of(shot))
    mcp.tool("move", x=10, y=20)
    pointer = ipc(sock, "pointer")["data"]
    check((pointer["x"], pointer["y"]) == (foot["x"] + 10, foot["y"] + 20), f"window shot mapping {pointer}")

    # Anything else through shady_ipc; errors are tool errors.
    check('"protocol":1' in text_of(mcp.tool("shady_ipc", cmd="version")), "shady_ipc version")
    check("Shady: unknown command" in text_of(mcp.tool("shady_ipc", ok=False, cmd="nope")), "bad cmd")
    mcp.tool("click", ok=False, x=1)
    mcp.tool("key", ok=False, keys="Ctrl+NoSuchKey")
    check(mcp.request("tools/call", {"name": "nope", "arguments": {}})["error"]["code"] == -32602,
          "unknown tool")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--mcp", default=str(ROOT / "build/shady-mcp"))
    parser.add_argument("--shady", default=shutil.which("shady"))
    parser.add_argument("--foot", default=shutil.which("foot"))
    args = parser.parse_args()
    for name, path in (("shady-mcp", args.mcp), ("shady", args.shady), ("foot", args.foot)):
        if not path or not Path(path).is_file():
            parser.error(f"cannot find {name}")

    # Short: a Unix socket path has at most 107 bytes.
    tmp = Path(tempfile.mkdtemp(prefix="smcp-", dir="/tmp"))
    try:
        runtime = tmp / "rt"
        runtime.mkdir(mode=0o700)
        out_file = tmp / "line.txt"
        config = tmp / "config.lua"
        config.write_text('shady.set("spatial_mode", false)\n')
        env = {k: v for k, v in os.environ.items()
               if k not in ("WAYLAND_DISPLAY", "WAYLAND_SOCKET", "SHADY_SOCKET")}
        env.update({"XDG_RUNTIME_DIR": str(runtime), "WLR_BACKENDS": "headless",
                    "WLR_HEADLESS_OUTPUTS": "1", "WLR_RENDERER": "pixman",
                    "XKB_DEFAULT_LAYOUT": "us", "SHADY_RICE": "none",
                    "ASAN_OPTIONS": "detect_leaks=0"})
        startup = f"{args.foot} -e sh -c 'IFS= read -r line; printf %s \"$line\" > {out_file}; sleep 30'"
        log_path = tmp / "shady.log"
        with open(log_path, "w") as log:
            comp = subprocess.Popen([args.shady, "--safe", "-c", str(config), "-s", startup],
                                    env=env, stdout=log, stderr=subprocess.STDOUT)
        sock = str(runtime / "shady-wayland-0.sock")
        ok = False
        try:
            def foot_mapped():
                check(comp.poll() is None, "shady exited early")
                try:
                    return any(w["app_id"] == "foot" for w in ipc(sock, "windows")["data"])
                except OSError:
                    return False
            wait_for(foot_mapped, "foot", timeout=15)
            mcp_env = {k: v for k, v in env.items() if k != "XDG_RUNTIME_DIR"}
            mcp_env["XDG_RUNTIME_DIR"] = str(runtime)  # finds the only Shady there
            mcp = Mcp([args.mcp], mcp_env)
            scenario(mcp, sock, out_file)
            check(mcp.close() == 0, "shady-mcp exited with an error")
            check(not any((runtime).glob("shady-mcp-*")), "screenshot directory left behind")
            ok = True
        except Fail as e:
            print(f"test-mcp: FAIL: {e}", file=sys.stderr)
            sys.stderr.write(log_path.read_text()[-4000:])
        finally:
            if comp.poll() is None:
                comp.terminate()
                try:
                    comp.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    comp.kill()
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    if not ok:
        return 1
    print("test-mcp: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
