#!/usr/bin/env python3
"""Capture World Adventures via ADB, optionally without restarting it."""

import argparse
from pathlib import Path
import subprocess
import time

PACKAGE = "com.ts3wa.port"


def run(args):
    adb = ["adb"] + (["-s", args.serial] if args.serial else [])
    args.output.mkdir(parents=True, exist_ok=True)
    stamp = time.strftime("%Y%m%d-%H%M%S")
    log = args.output / f"boot-{stamp}.log"
    screenshot = args.output / f"boot-{stamp}.png"
    crash_path = f"/sdcard/Android/data/{PACKAGE}/files/crash.txt"
    if not args.capture_only:
        subprocess.run(adb + ["shell", "am", "force-stop", PACKAGE], check=True)
        subprocess.run(adb + ["shell", f"if [ -f {crash_path} ]; then mv {crash_path} {crash_path}.before-{stamp}; fi"], check=True)
    with log.open("xb") as stream:
        capture = subprocess.Popen(adb + ["logcat", "-T", "1", "-v", "threadtime", "-s",
                                          "TS3WA:*", "TS3WA-crash:*", "TS3WA-NSLog:*",
                                          "TS3WA-printf:*", "DEBUG:*", "libc:*", "AndroidRuntime:*"],
                                   stdout=stream, stderr=subprocess.STDOUT)
        try:
            if not args.capture_only:
                subprocess.run(adb + ["shell", "am", "start", "-n", PACKAGE + "/.LauncherActivity"], check=True)
            time.sleep(args.seconds)
            with screenshot.open("xb") as output:
                subprocess.run(adb + ["exec-out", "screencap", "-p"], stdout=output, check=True)
        finally:
            capture.terminate()
            capture.wait(timeout=10)
    if subprocess.run(adb + ["shell", "test", "-f", crash_path]).returncode == 0:
        subprocess.run(adb + ["pull", crash_path, str(args.output / f"crash-{stamp}.txt")], check=True)
    print(f"Log: {log}\nScreenshot: {screenshot}")
    if log.stat().st_size == 0:
        # Some adbd versions stall the initial streaming request during app install.
        result = subprocess.run(adb + ["shell", "pidof", PACKAGE], capture_output=True, text=True)
        pid = result.stdout.strip().split()
        if pid:
            with log.open("ab") as stream:
                subprocess.run(adb + ["logcat", "-d", "--pid", pid[0], "-v", "threadtime", "-s",
                                      "TS3WA:*", "TS3WA-crash:*", "TS3WA-NSLog:*", "TS3WA-printf:*"],
                               stdout=stream, stderr=subprocess.STDOUT, check=True)
    for line in log.read_text(errors="replace").splitlines():
        if any(key in line for key in ("mapped ARM32", "rebase:", "bound ",
                                       "static initializer 28/", "game main", "UIApplicationMain",
                                       "UNIMPLEMENTED", "unrecognized selector", "fatal signal",
                                       "TS3WA-crash", "fps:")):
            print(line)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--serial")
    parser.add_argument("--seconds", type=int, default=20)
    parser.add_argument("--output", type=Path, default=Path("analysis/device"))
    parser.add_argument("--capture-only", action="store_true", help="Observe the current screen without restarting the game")
    run(parser.parse_args())
