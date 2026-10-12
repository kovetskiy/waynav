#!/usr/bin/env python3
"""Exercise persistent button clicks against Ghostty in an isolated compositor.

Start a headless compositor first and set XDG_RUNTIME_DIR to its private runtime
directory. The target terminal, navigation overlay, and virtual keyboard are
children of this script; the compositor itself is left running.
"""

import argparse
from contextlib import contextmanager
import os
from pathlib import Path
import re
import subprocess
import sys
import time


def wait_until(predicate, process, timeout=5):
    deadline = time.monotonic() + timeout
    while not predicate():
        if process.poll() is not None:
            raise RuntimeError(f"process exited with status {process.returncode}")
        if time.monotonic() >= deadline:
            raise TimeoutError("process did not become ready")
        time.sleep(0.02)


def stop(process):
    if process.poll() is None:
        process.terminate()
        try:
            process.wait(timeout=3)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()


@contextmanager
def target_terminal(arguments, result, environment, mode):
    mouse_log = result / "mouse.log"
    mouse_log.unlink(missing_ok=True)
    with (result / f"ghostty-{mode}.log").open("w") as terminal_log:
        terminal = subprocess.Popen([
            arguments.ghostty, "--config-default-files=false",
            "--gtk-single-instance=false", "--fullscreen=true",
            "--class=org.waynav.ClickSmoke", "-e", sys.executable,
            str(result / "receive-mouse.py"), str(mouse_log), mode,
        ], env=environment, stdout=terminal_log, stderr=terminal_log)
        try:
            wait_until(mouse_log.exists, terminal)
            yield
        finally:
            stop(terminal)


def run_navigation(arguments, result, environment, name, keys, ending=False):
    # One wtype process keeps the virtual keyboard present and supplies a
    # keymap containing every key used by the case.
    exit_keys = [] if ending else ["-k", "Escape"]
    keyboard = subprocess.Popen([
        arguments.wtype, "-s", "1000", *keys,
        "-s", "200", *exit_keys, "-s", "200",
    ], env=environment)
    try:
        time.sleep(0.1)
        with (result / f"{name}.log").open("w") as navigation_log:
            navigation = subprocess.Popen([
                arguments.waynav, "-c", str(result / "waynavrc"), "-l", "debug",
            ], env=environment, stdout=navigation_log, stderr=navigation_log)
            try:
                keyboard.wait(timeout=8)
                navigation.wait(timeout=3)
                assert keyboard.returncode == 0
                assert navigation.returncode == 0
            finally:
                stop(navigation)
    finally:
        stop(keyboard)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--waynav", required=True)
    parser.add_argument("--ghostty", default="ghostty")
    parser.add_argument("--wtype", default="wtype")
    parser.add_argument("--wl-copy", default="wl-copy")
    parser.add_argument("--wl-paste", default="wl-paste")
    parser.add_argument("--display", required=True)
    parser.add_argument("--result-dir", required=True, type=Path)
    arguments = parser.parse_args()
    result = arguments.result_dir.resolve()
    result.mkdir(parents=True, exist_ok=True)
    mouse_log = result / "mouse.log"
    mouse_log.unlink(missing_ok=True)
    receiver = result / "receive-mouse.py"
    receiver.write_text("""import os, sys, termios, tty
original = termios.tcgetattr(sys.stdin.fileno())
tty.setraw(sys.stdin.fileno())
try:
    if sys.argv[2] == 'report':
        os.write(sys.stdout.fileno(), b'\\x1b[?1000h\\x1b[?1006h')
    with open(sys.argv[1], 'ab', buffering=0) as log:
        while True:
            data = os.read(sys.stdin.fileno(), 4096)
            if not data:
                break
            log.write(data)
finally:
    termios.tcsetattr(sys.stdin.fileno(), termios.TCSANOW, original)
""")
    config = result / "waynavrc"
    config.write_text("""clear
semicolon start,grid 1x1
7 warp,click 1
8 warp,click 2
9 warp,click 3
q cut-left
Escape end
Return warp,click 1,end
F2 warp,click 2,end
F3 warp,click 3,end
""")
    environment = os.environ.copy()
    environment.update(WAYLAND_DISPLAY=arguments.display, GDK_BACKEND="wayland")
    with target_terminal(arguments, result, environment, "report"):
        cases = []
        for button, key, ending_key in [(0, "7", "Return"), (1, "8", "F2"), (2, "9", "F3")]:
            cases.append((f"button-{button}-navigate-click", [
                "-k", key, "-s", "200", "-k", "q", "-s", "200", "-k", key,
            ], button, False, False))
            cases.append((f"button-{button}-ending-click", ["-k", ending_key], button, True, False))
        cases.append(("held-click", [
            "-P", "7", "-s", "1100", "-p", "7", "-s", "200",
            "-k", "q", "-s", "200", "-k", "7",
        ], 0, False, True))
        for name, keys, button, ending, repeated in cases:
            mouse_log.write_bytes(b"")
            run_navigation(arguments, result, environment, name, keys, ending)
            data = mouse_log.read_bytes()
            prefix = rb"\x1b\[<" + str(button).encode()
            events = re.findall(prefix + rb";(\d+);(\d+)([Mm])", data)
            assert re.sub(prefix + rb";\d+;\d+[Mm]", b"", data) == b"", data
            assert len(events) >= (8 if repeated else 2), data
            assert len(events) % 2 == 0, events
            for press, release in zip(events[::2], events[1::2]):
                assert press[:2] == release[:2], events
                assert press[2] == b"M" and release[2] == b"m", events
            if not ending:
                assert int(events[-2][0]) < int(events[0][0]), events
            if not repeated:
                assert len(events) == (2 if ending else 4), events
            print(f"{name}: {len(events) // 2} complete clicks; passed", flush=True)

    with target_terminal(arguments, result, environment, "paste"):
        payload = b"waynav-primary-paste"
        clipboard = subprocess.Popen([
            arguments.wl_copy, "--primary", "--foreground",
        ], env=environment, stdin=subprocess.PIPE)
        try:
            clipboard.stdin.write(payload)
            clipboard.stdin.close()
            wait_until(lambda: subprocess.run([
                arguments.wl_paste, "--primary", "--no-newline",
            ], env=environment, capture_output=True).stdout == payload, clipboard)
            for name, key, ending in [("persistent-paste", "8", False), ("ending-paste", "F2", True)]:
                mouse_log.write_bytes(b"")
                run_navigation(arguments, result, environment, name, ["-k", key], ending)
                assert mouse_log.read_bytes() == payload, mouse_log.read_bytes()
                print(f"{name}: primary selection received; passed", flush=True)
        finally:
            stop(clipboard)


if __name__ == "__main__":
    main()
