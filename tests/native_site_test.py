"""Exercise the real website API against real C++ login/lobby EXEs in copied databases."""
from pathlib import Path
import argparse
import ctypes
import http.client as http_client
import json
import secrets
import shutil
import socket
import sqlite3
import struct
import subprocess
import sys
import threading
import time
from fixture_files import copytree_verified,copy_verified


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def free_port():
    with socket.socket() as listener:
        listener.bind(("127.0.0.1", 0))
        return listener.getsockname()[1]


def stop_native(process, prefix="RoshNativeStop_"):
    if process.poll() is not None:
        return
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.OpenEventW.argtypes = [ctypes.c_uint32, ctypes.c_int, ctypes.c_wchar_p]
    kernel.OpenEventW.restype = ctypes.c_void_p
    kernel.SetEvent.argtypes = [ctypes.c_void_p]
    kernel.CloseHandle.argtypes = [ctypes.c_void_p]
    event = kernel.OpenEventW(2, False, f"Local\\{prefix}{process.pid}")
    if event:
        kernel.SetEvent(event)
        kernel.CloseHandle(event)
    process.wait(timeout=12)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("package", type=Path)
    parser.add_argument("fixture", type=Path)
    args = parser.parse_args()
    package, fixture = args.package.resolve(), args.fixture.resolve()
    require(not fixture.exists(), "Fixture must be a new directory")
    (fixture / "Database").mkdir(parents=True)
    (fixture / "Config/Keys").mkdir(parents=True)
    copytree_verified(package / "Data", fixture / "Data")
    copy_verified(package / "Config/Keys/content.key.dpapi", fixture / "Config/Keys/content.key.dpapi")
    for file in (package / "Database").glob("*.sqlite"):
        if file.name == "runtime.sqlite":
            continue
        with sqlite3.connect(file.as_uri() + "?mode=ro", uri=True) as source:
            with sqlite3.connect(fixture / "Database" / file.name) as destination:
                source.backup(destination)
    gate_port = free_port()
    (fixture / "Config/server.json").write_text(json.dumps({"login_port": gate_port, "site_port": 0}), encoding="utf-8")
    sys.path.insert(0, str(package / "SITE"))
    from core_site import CppPortalServer
    processes = []
    web_process = None
    server = thread = None
    try:
        for name in ("LobbyServer", "LoginServer"):
            processes.append(subprocess.Popen([str(package / "Bin" / (name + ".exe")), "--root", str(fixture),
                                               "--headless", "--start", "--duration", "90"],
                                              creationflags=subprocess.CREATE_NO_WINDOW))
        deadline = time.monotonic() + 15
        while True:
            require(all(p.poll() is None for p in processes), "A native login process exited")
            try:
                with socket.create_connection(("127.0.0.1", gate_port), timeout=.5) as connection:
                    connection.sendall(b"KAROS-LOCAL-STATUS\n")
                    if connection.recv(64) == b"KAROS-TEST-FUNCTIONS-AUTH/1\n":
                        break
            except OSError:
                pass
            require(time.monotonic() < deadline, "Native login startup timed out")
            time.sleep(.1)
        server = CppPortalServer(fixture, 0)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()

        def http(path, body=None, cookie=""):
            connection = http_client.HTTPConnection("127.0.0.1", server.server_port, timeout=8)
            headers = {"Origin": server.origin, "Content-Type": "application/json"}
            if cookie:
                headers["Cookie"] = cookie
            connection.request("POST" if body is not None else "GET", path,
                               json.dumps(body).encode() if body is not None else None, headers)
            response = connection.getresponse()
            payload = response.read()
            result = response.status, dict(response.getheaders()), payload
            connection.close()
            return result

        def native(username, credential, expected):
            fields = (username, credential, "web-native-1", "web-native-2", "web-native-3")
            body = bytes(271) + b"".join(field.encode("ascii") + b"\0" for field in fields)
            frame = struct.pack(">I", len(body) + 6) + b"\x02\0\0\0\0\x01" + body
            with socket.create_connection(("127.0.0.1", gate_port), timeout=8) as connection:
                connection.sendall(frame)

                def read(size):
                    result = b""
                    while len(result) < size:
                        part = connection.recv(size - len(result))
                        require(bool(part), "Native login connection closed")
                        result += part
                    return result

                packet = read(struct.unpack(">I", read(4))[0])
                require(tuple(packet[:2]) == expected, "Native login did not accept the website account/ticket as expected")
                if expected == (11, 0):
                    for opcode in ((2, 0), (2, 6)):
                        packet = read(struct.unpack(">I", read(4))[0])
                        require(tuple(packet[:2]) == opcode, "Native lobby handoff missing")

        status, _, raw = http("/api/status")
        info = json.loads(raw)
        require(status == 200 and info["core_backend"] == "cpp" and info["native_game_login"], "Website is not bound to the running native core")
        require(http("/")[0] == 200 and http("/api/library")[0] == 200 and http("/api/skills")[0] == 200,
                "Existing website pages, guide or skills are unavailable")
        username = "CppWeb" + secrets.token_hex(5)
        password = secrets.token_urlsafe(20)
        require(http("/api/register", {"username": username, "password": password})[0] == 201, "Website registration failed")
        status, headers, _ = http("/api/login", {"username": username, "password": password})
        require(status == 200, "Website login failed")
        cookie = headers["Set-Cookie"].split(";", 1)[0]
        status, _, raw = http("/api/me", cookie=cookie)
        require(status == 200 and json.loads(raw)["characters"] == [], "New website account gained another account's characters")
        require(http("/admin", cookie=cookie)[0] == 403, "Ordinary account gained admin access")
        require(http("/api/launcher", cookie=cookie)[0] == 200, "Dashboard API unavailable")
        # Original Mode: username/password enter through the real native gate.
        native(username, password, (11, 0))
        native(username, "wrong-password-42", (11, 1))
        # Dashboard: website session issues a one-use ticket consumed by the real C++ gate.
        status, _, raw = http("/api/game-session", {}, cookie)
        require(status == 200, "Website could not issue a native launch ticket")
        ticket = json.loads(raw)
        native(ticket["username"], "L1:" + ticket["launch_secret"], (11, 0))
        native(ticket["username"], "L1:" + ticket["launch_secret"], (11, 1))
        status, _, raw = http("/api/game-session", {}, cookie)
        pending = json.loads(raw)
        require(http("/api/logout", {}, cookie)[0] == 200, "Website logout failed")
        native(pending["username"], "L1:" + pending["launch_secret"], (11, 1))
        require(http("/api/me", cookie=cookie)[0] == 401, "Revoked website session remains active")
        # Also exercise the packaged standalone entry point and graceful shutdown event.
        standalone_port = free_port()
        web_process = subprocess.Popen([str(package / "SITE/runtime/python.exe"), "-B", "-E", "-s",
                                        str(package / "SITE/core_site.py"), "--core-root", str(fixture),
                                        "--port", str(standalone_port)], creationflags=subprocess.CREATE_NO_WINDOW,
                                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        deadline = time.monotonic() + 10
        while True:
            require(web_process.poll() is None, "Packaged website entry point exited")
            try:
                connection = http_client.HTTPConnection("127.0.0.1", standalone_port, timeout=1)
                connection.request("GET", "/api/status")
                response = connection.getresponse()
                state = json.loads(response.read())
                connection.close()
                if response.status == 200 and state.get("core_backend") == "cpp":
                    break
            except (OSError, ValueError):
                pass
            require(time.monotonic() < deadline, "Packaged website entry point timed out")
            time.sleep(.1)
        print("PASS: website pages/guide/skills; shared C++ accounts; isolated characters; protected admin; Dashboard API; native password login; website ticket -> real C++ login; ticket replay and logout revocation", flush=True)
    finally:
        if server:
            server.shutdown()
            server.server_close()
        if thread:
            thread.join(timeout=5)
        if web_process:
            stop_native(web_process, "RoshSiteStop_")
        for process in processes:
            stop_native(process)


if __name__ == "__main__":
    main()
