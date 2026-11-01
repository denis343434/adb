"""Standalone ad helper: watch an ad, then press its Skip / Close button.

This script is NOT started by the autoplay: the app has the same detector built
in (farm/automation/adbuttonfinder.cpp) and taps the Skip / Close control itself.
Run this script manually when you want to close ads from the desktop:

    python watch_ads.py --devices <serial>[,<serial>...] [--watch 15]

The script lets the ad play for `--watch` seconds (so a rewarded ad can reach
its end), then looks for the Skip / Close button and taps it.  It repeats while
the button is still on screen, so a button that needs a second press (or an ad
with both a skip pill and a close cross) is handled too.

Only one button is pressed per pass — the best match — and only in the corner
zones where ad controls live.  That keeps the script from "clicking through" the
ad creative and opening the advertiser's page.
"""
import os
import shutil
import subprocess
import sys
import time

import cv2
import numpy as np

BUTTONS_FOLDER = "buttons"          # button samples (crops), shared with skip_ads.py
THRESHOLD_SKIP = 0.88               # minimal score for skip buttons
THRESHOLD_CLOSE = 0.92              # close crosses are easier to confuse, be strict
RIGHT_FRACTION = 0.66               # ad controls live on the right side
TOP_FRACTION = 0.40                 # ... in the top-right corner
BOTTOM_FRACTION = 0.60              # ... or in the bottom-right strip
BANNER_FRACTION = 0.87              # never below this: the in-game banner lives there
WORK_WIDTH = 540                    # screenshots are downscaled to this width
SCALES = np.linspace(0.4, 2.5, 22)  # template sizes to try
CHECK_INTERVAL = 0.8                # seconds between screenshots
TAP_PAUSE = 0.8                     # pause after a tap before looking again
WATCH_SECONDS = 15.0                # let the ad play this long before touching it

IMAGE_EXTENSIONS = (".png", ".jpg", ".jpeg", ".bmp", ".webp")


def read_image(path, flags=cv2.IMREAD_COLOR):
    # cv2.imread cannot open paths with Cyrillic letters on Windows.
    return cv2.imdecode(np.fromfile(path, np.uint8), flags)


def find_adb():
    found = os.environ.get("ADB")
    if found and os.path.isfile(found):
        return found
    adb = shutil.which("adb")
    if adb:
        return adb
    default = os.path.expandvars(r"%LOCALAPPDATA%\Android\Sdk\platform-tools\adb.exe")
    if os.path.isfile(default):
        return default
    print("adb not found — add adb to PATH", flush=True)
    raise SystemExit(1)


def load_templates(folder=BUTTONS_FOLDER):
    skips = []
    closes = []
    if not os.path.isdir(folder):
        print(f"no folder {folder}", flush=True)
        raise SystemExit(1)
    for name in sorted(os.listdir(folder)):
        if not name.lower().endswith(IMAGE_EXTENSIONS):
            continue
        image = read_image(os.path.join(folder, name), cv2.IMREAD_GRAYSCALE)
        if image is None:
            continue
        if name.lower().startswith("skip"):
            skips.append((name, image))
        elif name.lower().startswith("close"):
            closes.append((name, image))
    if not skips and not closes:
        print(f"no button samples in {folder}", flush=True)
        raise SystemExit(1)
    return skips, closes


def get_devices(adb, wanted):
    output = subprocess.run([adb, "devices"], capture_output=True, text=True).stdout
    devices = [line.split()[0] for line in output.splitlines()[1:] if line.endswith("\tdevice")]
    return [device for device in devices if device in wanted]


def take_screenshot(adb, device):
    data = subprocess.run([adb, "-s", device, "exec-out", "screencap", "-p"], capture_output=True).stdout
    if not data:
        return None
    return cv2.imdecode(np.frombuffer(data, np.uint8), cv2.IMREAD_COLOR)


def tap(adb, device, x, y):
    subprocess.run([adb, "-s", device, "shell", "input", "tap", str(x), str(y)])


def find_button(screenshot, templates, threshold):
    """Best Skip/Close match in the ad-control zones, or None."""
    height, width = screenshot.shape[:2]
    factor = WORK_WIDTH / width
    gray = cv2.cvtColor(screenshot, cv2.COLOR_BGR2GRAY)
    gray = cv2.resize(gray, None, fx=factor, fy=factor, interpolation=cv2.INTER_AREA)
    work_height, work_width = gray.shape

    top_limit = int(height * TOP_FRACTION)
    bottom_from = int(height * BOTTOM_FRACTION)
    banner_top = int(height * BANNER_FRACTION)
    right_from = int(width * RIGHT_FRACTION)

    best = None
    for name, template in templates:
        for scale in SCALES:
            resized = cv2.resize(template, None, fx=scale, fy=scale)
            h, w = resized.shape
            if min(h, w) < 5 or h > work_height or w > work_width:
                continue
            result = cv2.matchTemplate(gray, resized, cv2.TM_CCOEFF_NORMED)
            _, score, _, (x, y) = cv2.minMaxLoc(result)
            if score < threshold:
                continue
            cx = (x + w / 2) / factor
            cy = (y + h / 2) / factor
            if cx < right_from or cy > banner_top:
                continue
            if not (cy < top_limit or cy > bottom_from):
                continue
            if best is None or score > best[2]:
                best = (int(cx), int(cy), float(score))
    return best


def run(adb, devices, skips, closes, watch_seconds):
    first_seen = {}
    print(f"Watching ads on: {', '.join(devices)}", flush=True)
    print(f"First {watch_seconds:g} s: watching; then tap Skip/Close.", flush=True)

    while True:
        for device in devices:
            now = time.time()
            first_seen.setdefault(device, now)
            if now - first_seen[device] < watch_seconds:
                continue  # the ad is still being watched

            screenshot = take_screenshot(adb, device)
            if screenshot is None:
                continue

            found = find_button(screenshot, skips, THRESHOLD_SKIP)
            if found is None:
                found = find_button(screenshot, closes, THRESHOLD_CLOSE)
            if found is None:
                continue

            x, y, score = found
            tap(adb, device, x, y)
            print(f"[{time.strftime('%H:%M:%S')}] {device}: tapped ({x}, {y}), score {score:.2f}", flush=True)
            time.sleep(TAP_PAUSE)

        time.sleep(CHECK_INTERVAL)


def main():
    os.chdir(os.path.dirname(os.path.abspath(__file__)))

    wanted = set()
    if "--devices" in sys.argv:
        index = sys.argv.index("--devices")
        if index + 1 < len(sys.argv):
            wanted = {name for name in sys.argv[index + 1].split(",") if name}
    if not wanted:
        print("need a device list: --devices <serial,...>", flush=True)
        return 1

    watch_seconds = WATCH_SECONDS
    if "--watch" in sys.argv:
        index = sys.argv.index("--watch")
        if index + 1 < len(sys.argv):
            try:
                watch_seconds = float(sys.argv[index + 1])
            except ValueError:
                pass

    adb = find_adb()
    skips, closes = load_templates()
    run(adb, sorted(wanted), skips, closes, watch_seconds)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
