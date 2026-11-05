# SPDX-License-Identifier: GPL-3.0-or-later
"""One visible enhanced-game run of the native renderer on the host device.

No benchmarks, repeated runs or standalone probes. Arm only at the in-level
player_ready checkpoint; preserve the selected save and settings as inputs.
"""
import argparse
import configparser
import ctypes as c
from ctypes import wintypes as w
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import time

from prepare_native_mesh import private_output, require


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def window_proof(pid, destination):
    user = c.WinDLL("user32", use_last_error=True)
    user.SetProcessDpiAwarenessContext.argtypes = [c.c_void_p]
    user.SetProcessDpiAwarenessContext(c.c_void_p(-4))
    user.GetWindowThreadProcessId.argtypes = [w.HWND, c.POINTER(w.DWORD)]
    user.IsWindowVisible.argtypes = [w.HWND]
    user.IsZoomed.argtypes = [w.HWND]
    user.GetClientRect.argtypes = [w.HWND, c.POINTER(w.RECT)]
    user.GetWindowRect.argtypes = [w.HWND, c.POINTER(w.RECT)]
    user.SetWindowPos.argtypes = [w.HWND, w.HWND, c.c_int, c.c_int, c.c_int, c.c_int, w.UINT]
    callback = c.WINFUNCTYPE(w.BOOL, w.HWND, w.LPARAM)
    found = []

    @callback
    def visit(hwnd, _):
        owner = w.DWORD()
        user.GetWindowThreadProcessId(hwnd, c.byref(owner))
        if owner.value == pid and user.IsWindowVisible(hwnd):
            rect = w.RECT()
            user.GetClientRect(hwnd, c.byref(rect))
            if (rect.right, rect.bottom) == (1280, 720):
                found.append(hwnd)
        return True

    user.EnumWindows.argtypes = [callback, w.LPARAM]
    user.EnumWindows(visit, 0)
    if not found:
        return False
    require(len(found) == 1 and not user.IsZoomed(found[0]), "Expected one normal window")
    hwnd = found[0]
    require(user.SetWindowPos(hwnd, None, 100, 100, 0, 0, 1 | 4 | 16), "Cannot place test window")
    rect = w.RECT()
    require(user.GetWindowRect(hwnd, c.byref(rect)), "Cannot inspect test window")
    require(rect.right - rect.left > 1280 and rect.bottom - rect.top > 720, "Window is borderless/fullscreen")
    destination.write_text(json.dumps({"pid": pid, "hwnd": int(hwnd), "visible": True,
        "client": [1280, 720], "outer": [rect.left, rect.top, rect.right, rect.bottom], "maximized": False}, indent=2))
    return True


def run(args):
    repo = Path(__file__).resolve().parents[1]
    work = private_output(args.work)
    config = configparser.ConfigParser()
    config.read(args.settings)
    require(config["Display"]["window_mode"] == "windowed" and
            (config["Display"].getint("window_width"), config["Display"].getint("window_height")) == (1280, 720),
            "Use explicit 1280x720 windowed settings")
    query = "@(Get-Process ps2EntryRunner,cl,link,ninja,cmake -ErrorAction SilentlyContinue | Select-Object Id,ProcessName) | ConvertTo-Json -Compress"
    require(not subprocess.check_output(["powershell.exe", "-NoProfile", "-Command", query], text=True).strip(),
            "Game or compilation already active")
    protected = [args.settings, *sorted(p for p in args.seed.rglob("*") if p.is_file())]
    before = {str(p): sha(p) for p in protected}
    options = {}
    for line in (repo / "launch/enhanced_env.txt").read_text().splitlines():
        if line and not line.startswith("#"):
            key, value = line.split("=", 1)
            options[key] = value
    options.update(PS2X_SETTINGS=str(args.settings), PS2X_TF_GPU_GRASS_BATCHES="1",
        PS2X_RESOPT2_HOST_SEQUENCE="0:1280x720@60:1", PS2X_PCMENU_AUTOTEST="1000000000",
        PS2X_EE_PERF_LOG="0", PS2X_EETRIM_STATS="0",
        PS2X_TF_NATIVE_HOST_PROBE=str(work / "host-probe"), PS2X_TF_NATIVE_HOST_ARM=str(work / "native-arm.txt"),
        PS2X_TF_NATIVE_HOST_ASSET=str(args.asset / "asset.bin"), PS2X_TF_NATIVE_HOST_SHADER=str(args.shader))
    command = ["powershell.exe", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", str(repo / "testing/run_test.ps1"),
        "-Exe", str(args.exe), "-Elf", str(args.elf), "-Iso", str(args.iso),
        "-Script", str(repo / "testing/enhancements/native_mesh_host.tfs"), "-Work", str(work),
        "-Seconds", "600", "-ProcessPriority", "Normal", "-McSeed", str(args.seed),
        "-Env", ";".join(f"{k}={v}" for k, v in options.items())]
    env = {k: v for k, v in os.environ.items() if not k.startswith(("PS2X_", "KZGS_"))}
    started, pid, checked, armed = time.monotonic(), None, False, False
    next_pid_query = started
    child = subprocess.Popen(command, env=env, cwd=repo)
    print(f"Started one enhanced host-probe route, wrapper PID {child.pid}", flush=True)
    try:
        while child.poll() is None:
            now = time.monotonic()
            require(now - started < 630, "Host route wrapper did not honor its timeout")
            if pid is None and now >= next_pid_query:
                query = f'Get-CimInstance Win32_Process -Filter "ParentProcessId={child.pid} AND Name=\'ps2EntryRunner.exe\'" | Select-Object -ExpandProperty ProcessId'
                value = subprocess.check_output(["powershell.exe", "-NoProfile", "-Command", query], text=True).strip()
                if value.isdigit():
                    pid = int(value)
                next_pid_query = now + 1
            if pid and not checked and work.exists():
                checked = window_proof(pid, work / "window-proof.json")
            log = work / "stderr.log"
            text = log.read_text(errors="replace") if log.exists() else ""
            if not armed and "[script] mark native_probe_begin vs=" in text:
                require(checked, "Refusing to arm without a visible window")
                (work / "native-arm.txt").write_text("Stable in-level player_ready checkpoint reached\n")
                armed = True
                print("Native probe armed inside the level", flush=True)
            time.sleep(0.2)
    finally:
        if child.poll() is None:
            # Stop only this invocation's process tree on setup/arming failure.
            subprocess.run(["taskkill.exe", "/PID", str(child.pid), "/T", "/F"],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            child.wait(timeout=15)
    result = {"exit_code": child.returncode, "run_limit": 1, "game_pid": pid, "window_verified": checked,
              "armed_after_player_ready": armed, "scope": "In-process correctness; no performance comparison",
              "exe_sha256": sha(args.exe), "dll_sha256": sha(args.exe.parent / "tf_hwgs.dll"),
              "protected_inputs_unchanged": all(sha(Path(p)) == h for p, h in before.items())}
    (work / "host-run.json").write_text(json.dumps(result, indent=2) + "\n")
    require(child.returncode == 0 and checked and armed and result["protected_inputs_unchanged"], "Host run failed")
    subprocess.run([sys.executable, str(repo / "testing/verify_native_mesh_host.py"),
                    "--asset", str(args.asset), "--work", str(work)], check=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("exe", "elf", "iso", "seed", "settings", "asset", "shader", "work"):
        parser.add_argument("--" + name, type=lambda v: Path(v).resolve(), required=True)
    run(parser.parse_args())
