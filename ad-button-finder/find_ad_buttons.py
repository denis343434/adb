#!/usr/bin/env python3
"""Find the Skip (and later Close) button of a full-screen ad with OpenCV.

This is a standalone desktop tool: it is not part of the device-farm player
and nothing in the player calls it.  Point it at a screenshot or at a
connected phone and it prints where the button is.  With --tap it presses the
best match via adb.

Examples
--------
    python find_ad_buttons.py --image ad.png --save ad.marked.png
    python find_ad_buttons.py --device EIPBNFCU6PRWKF5T
    python find_ad_buttons.py --device EIPBNFCU6PRWKF5T --tap
    python find_ad_buttons.py --image ad.png --show
    python find_ad_buttons.py --selftest

adb is bundled in platform-tools/, so no system-wide ADB setup is needed.

How it works
------------
The crop in templates/skip.png is auto-cropped to the icon itself and matched
at many scales (the same crop then works when an ad draws the button bigger or
smaller).  Matches are limited to the right side of the screen and kept away
from the permanent in-game banner at the bottom.  A match must also beat the
threshold; the top candidate is printed with its score so a bad threshold is
easy to spot.

Templates are PNG/JPG files in templates/ named skip*.png, close*.png.  The
file name decides the kind; add more crops over time, they are all tried.
"""
from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
import time
from dataclasses import dataclass
from pathlib import Path

import cv2
import numpy as np

SCRIPT_DIR = Path(__file__).resolve().parent
TEMPLATES_DIR = SCRIPT_DIR / "templates"
IMAGE_EXTENSIONS = (".png", ".jpg", ".jpeg", ".bmp", ".webp")

DEFAULT_THRESHOLD = 0.85
DEFAULT_MIN_SCALE = 1.0
DEFAULT_MAX_SCALE = 2.6
DEFAULT_SCALE_STEP = 0.05

# A full-screen ad keeps its controls on the right, never in the in-game banner.
RIGHT_FRACTION = 0.55
TOP_FRACTION = 0.35
BOTTOM_FRACTION = 0.55
BANNER_FRACTION = 0.88       # anything below this is the permanent game banner
KEEP_MARGIN = 2              # pixels of background kept around a cropped icon
BG_DELTA = 10                # grey-level difference from the template background


@dataclass
class Template:
    name: str
    kind: str
    image: np.ndarray


@dataclass
class Detection:
    kind: str
    template: str
    x: int
    y: int
    score: float
    scale: float
    box: tuple[int, int, int, int]   # x, y, w, h in screenshot pixels

    def as_dict(self) -> dict:
        return {
            "what": self.kind,
            "template": self.template,
            "x": self.x,
            "y": self.y,
            "score": round(self.score, 4),
            "scale": round(self.scale, 3),
            "box": list(self.box),
        }


def read_image(path, flags=cv2.IMREAD_COLOR):
    """cv2.imread cannot open paths with Cyrillic letters on Windows."""
    return cv2.imdecode(np.fromfile(str(path), np.uint8), flags)


def to_gray(image: np.ndarray) -> np.ndarray:
    if image is None:
        return image
    if image.ndim == 3 and image.shape[2] == 4:
        image = cv2.cvtColor(image, cv2.COLOR_BGRA2BGR)
    if image.ndim == 3:
        return cv2.cvtColor(image, cv2.COLOR_BGR2GRAY)
    return image


def autocrop(gray: np.ndarray, margin: int = KEEP_MARGIN, bg_delta: int = BG_DELTA) -> np.ndarray:
    """Trim the flat background around a cropped icon.

    A template that is mostly white is dominated by that white in
    TM_CCOEFF_NORMED, which lowers the score of a real match and lets flat
    areas win.  Cropping to the icon plus a couple of pixels fixes that and
    also keeps a coloured background template (the icon is what differs from
    the border colour) working.
    """
    height, width = gray.shape[:2]
    if height < 4 or width < 4:
        return gray
    border = np.concatenate([gray[0, :], gray[-1, :], gray[:, 0], gray[:, -1]])
    background = int(np.median(border))
    mask = np.abs(gray.astype(np.int16) - background) > bg_delta
    ys, xs = np.where(mask)
    if len(xs) == 0:
        return gray
    x0 = max(0, int(xs.min()) - margin)
    x1 = min(width, int(xs.max()) + margin + 1)
    y0 = max(0, int(ys.min()) - margin)
    y1 = min(height, int(ys.max()) + margin + 1)
    cropped = gray[y0:y1, x0:x1]
    # If the "background" is not actually flat (a photo), the crop would be
    # almost the whole template; keep the original then.
    if cropped.size < 0.15 * gray.size:
        return gray
    return cropped


def load_templates(what: str) -> list[Template]:
    if not TEMPLATES_DIR.is_dir():
        raise SystemExit(f"no templates folder: {TEMPLATES_DIR}")
    wanted = ("skip", "close") if what == "all" else (what,)
    templates: list[Template] = []
    for path in sorted(TEMPLATES_DIR.iterdir()):
        if path.suffix.lower() not in IMAGE_EXTENSIONS:
            continue
        kind = path.stem.lower().split("_")[0]
        if kind not in wanted:
            continue
        image = read_image(path, cv2.IMREAD_GRAYSCALE)
        if image is None:
            print(f"warning: cannot read {path.name}", file=sys.stderr)
            continue
        templates.append(Template(name=path.name, kind=kind, image=autocrop(image)))
    if not templates:
        example = "skip" if what == "all" else what
        raise SystemExit(
            f"no {what}*.png templates in {TEMPLATES_DIR}\n"
            f"put the button crop there (for example templates/{example}.png)"
        )
    return templates


def region_ok(cx: float, cy: float, width: int, height: int, region: str,
              banner: float = BANNER_FRACTION) -> bool:
    """Is this point in the part of the screen where the ad controls live?"""
    if cy >= height * banner:
        return False
    if region == "any":
        return True
    if cx < width * RIGHT_FRACTION:
        return False
    if region == "top-right":
        return cy <= height * TOP_FRACTION
    if region == "bottom-right":
        return cy >= height * BOTTOM_FRACTION
    return True  # "right": top-right or bottom-right, above the banner


def match_template(gray: np.ndarray, template: Template, threshold: float,
                   scales: np.ndarray, region: str) -> list[Detection]:
    """All matches of one template above `threshold` on one grayscale screen."""
    height, width = gray.shape[:2]
    found: list[Detection] = []
    for scale in scales:
        interpolation = cv2.INTER_AREA if scale < 1.0 else cv2.INTER_LINEAR
        resized = cv2.resize(template.image, None, fx=scale, fy=scale,
                             interpolation=interpolation)
        th, tw = resized.shape[:2]
        if min(th, tw) < 6 or th > height or tw > width:
            continue
        result = cv2.matchTemplate(gray, resized, cv2.TM_CCOEFF_NORMED)
        mask = (result >= threshold).astype(np.uint8)
        if not mask.any():
            continue
        count, labels, stats, _ = cv2.connectedComponentsWithStats(mask, connectivity=8)
        for label in range(1, count):
            left, top, w, h, _area = stats[label]
            patch = result[top:top + h, left:left + w]
            if patch.size == 0:
                continue
            _, score, _, best = cv2.minMaxLoc(patch)
            x = left + best[0]
            y = top + best[1]
            cx = x + tw / 2.0
            cy = y + th / 2.0
            if not region_ok(cx, cy, width, height, region):
                continue
            found.append(Detection(kind=template.kind, template=template.name,
                                   x=int(round(cx)), y=int(round(cy)),
                                   score=float(score), scale=float(scale),
                                   box=(int(x), int(y), int(tw), int(th))))
    return found


def suppress(candidates: list[Detection]) -> list[Detection]:
    """Keep the best box of every cluster (the same button found at 2 scales)."""
    kept: list[Detection] = []
    for candidate in sorted(candidates, key=lambda d: d.score, reverse=True):
        duplicate = False
        for other in kept:
            dx = candidate.x - other.x
            dy = candidate.y - other.y
            reach = max(10, int(0.6 * min(other.box[2], other.box[3])))
            if dx * dx + dy * dy < reach * reach:
                duplicate = True
                break
        if not duplicate:
            kept.append(candidate)
    return kept


def find_buttons(gray: np.ndarray, templates: list[Template], threshold: float,
                 scales: np.ndarray, region: str) -> list[Detection]:
    candidates: list[Detection] = []
    for template in templates:
        candidates.extend(match_template(gray, template, threshold, scales, region))
    return suppress(candidates)


def annotate(image: np.ndarray, detections: list[Detection]) -> np.ndarray:
    marked = image.copy()
    if marked.ndim == 2:
        marked = cv2.cvtColor(marked, cv2.COLOR_GRAY2BGR)
    elif marked.shape[2] == 4:
        marked = cv2.cvtColor(marked, cv2.COLOR_BGRA2BGR)
    height, width = marked.shape[:2]
    # Show the searched band so a wrong crop/threshold is obvious.
    cv2.rectangle(marked, (int(width * RIGHT_FRACTION), 0),
                  (width - 1, int(height * BANNER_FRACTION)), (120, 120, 120), 1)
    for index, det in enumerate(detections):
        x, y, w, h = det.box
        colour = (0, 220, 0) if index == 0 else (0, 160, 255)
        cv2.rectangle(marked, (x, y), (x + w, y + h), colour, 2)
        cv2.drawMarker(marked, (det.x, det.y), (0, 0, 255), cv2.MARKER_CROSS, 22, 2)
        label = f"{det.kind} {det.score:.2f} sc{det.scale:.2f}"
        (label_w, label_h), _ = cv2.getTextSize(label, cv2.FONT_HERSHEY_SIMPLEX, 0.7, 2)
        label_x = max(2, min(x, width - label_w - 4))
        label_y = y - 8 if y - 8 > label_h else y + h + label_h + 6
        cv2.putText(marked, label, (label_x, label_y), cv2.FONT_HERSHEY_SIMPLEX,
                    0.7, colour, 2, cv2.LINE_AA)
    return marked


def show_image(image: np.ndarray, detections: list[Detection]) -> None:
    try:
        cv2.imshow("ad button finder", annotate(image, detections))
        cv2.waitKey(0)
        cv2.destroyAllWindows()
    except cv2.error as error:
        print(f"cannot open a window: {error}", file=sys.stderr)


def find_adb(explicit: str | None = None) -> str:
    if explicit:
        if Path(explicit).is_file():
            return explicit
        raise SystemExit(f"adb not found at --adb path: {explicit}")
    env = os.environ.get("ADB")
    if env and Path(env).is_file():
        return env
    # The tool ships its own adb so it works without a system-wide setup.
    bundled = SCRIPT_DIR / "platform-tools" / "adb.exe"
    if bundled.is_file():
        return str(bundled)
    found = shutil.which("adb")
    if found:
        return found
    sdk = os.path.expandvars(r"%LOCALAPPDATA%\Android\Sdk\platform-tools\adb.exe")
    if Path(sdk).is_file():
        return sdk
    user_tools = Path(os.environ.get("USERPROFILE", "")) / "platform-tools" / "adb.exe"
    if user_tools.is_file():
        return str(user_tools)
    raise SystemExit("adb not found: put adb in platform-tools/ or pass --adb PATH")


def adb_devices(adb: str) -> list[str]:
    output = subprocess.run([adb, "devices"], capture_output=True, text=True).stdout
    return [line.split()[0] for line in output.splitlines()[1:]
            if line.rstrip().endswith("\tdevice")]


def take_screenshot(adb: str, serial: str) -> np.ndarray:
    data = subprocess.run([adb, "-s", serial, "exec-out", "screencap", "-p"],
                          capture_output=True).stdout
    if not data:
        raise SystemExit(f"{serial}: screencap returned nothing")
    image = cv2.imdecode(np.frombuffer(data, np.uint8), cv2.IMREAD_COLOR)
    if image is None:
        raise SystemExit(f"{serial}: screencap returned no image")
    return image


def tap(adb: str, serial: str, x: int, y: int) -> None:
    subprocess.run([adb, "-s", serial, "shell", "input", "tap", str(x), str(y)])


def save_image(path: Path, image: np.ndarray) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    extension = path.suffix.lower() or ".png"
    ok, encoded = cv2.imencode(extension, image)
    if not ok:
        raise SystemExit(f"cannot encode {path}")
    encoded.tofile(str(path))


def parse_scales(args) -> np.ndarray:
    if args.min_scale <= 0 or args.max_scale < args.min_scale or args.scale_step <= 0:
        raise SystemExit("bad --min-scale/--max-scale/--scale-step")
    count = int(round((args.max_scale - args.min_scale) / args.scale_step)) + 1
    return np.linspace(args.min_scale, args.max_scale, max(1, count))


def selftest(threshold: float) -> int:
    """Every template must be found on its own background; blank screens stay clean.

    The background is the template's own border colour, so both the light
    (grey-on-white) and the dark (white-on-black) crops get a realistic test.
    """
    templates = load_templates("skip")
    scales = np.linspace(1.0, 2.6, 33)
    x, y = 610, 120
    for index, template in enumerate(templates):
        border = template.image
        border = int(np.median(np.concatenate(
            [border[0, :], border[-1, :], border[:, 0], border[:, -1]])))
        base = np.full((1600, 720, 3), border, np.uint8)
        # A slightly different band behind the button: a pure flat screen would
        # be an unrealistically easy match.
        cv2.rectangle(base, (0, 0), (719, 210),
                      (min(255, border + 12),) * 3, -1)
        icon = cv2.resize(template.image, None, fx=2.0, fy=2.0)
        base[y:y + icon.shape[0], x:x + icon.shape[1]] = cv2.cvtColor(icon, cv2.COLOR_GRAY2BGR)
        noise = np.random.default_rng(index).normal(0, 3, base.shape).astype(np.int16)
        noisy = np.clip(base.astype(np.int16) + noise, 0, 255).astype(np.uint8)

        found = find_buttons(to_gray(noisy), templates, threshold, scales, "right")
        expected = (x + icon.shape[1] // 2, y + icon.shape[0] // 2)
        if not found:
            print(f"SELFTEST FAIL: {template.name}: no match (expected near {expected})")
            return 1
        best = found[0]
        distance = ((best.x - expected[0]) ** 2 + (best.y - expected[1]) ** 2) ** 0.5
        if distance > 6:
            print(f"SELFTEST FAIL: {template.name}: best {best.x},{best.y} is "
                  f"{distance:.1f} px from {expected}")
            return 1
        if best.template != template.name:
            print(f"SELFTEST FAIL: {template.name}: matched {best.template} instead")
            return 1
        print(f"SELFTEST PASS: {template.name}: found {best.x},{best.y} "
              f"(expected {expected[0]},{expected[1]}), score {best.score:.2f}, "
              f"scale {best.scale:.2f}")

    empty = np.full((1600, 720, 3), 250, np.uint8)
    if find_buttons(to_gray(empty), templates, threshold, scales, "right"):
        print("SELFTEST FAIL: blank screen produced a match")
        return 1
    print("SELFTEST PASS: blank screen clean")
    return 0


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(
        description="Find an ad's Skip/Close button with OpenCV template matching.")
    source = parser.add_mutually_exclusive_group()
    source.add_argument("--image", type=Path, help="screenshot to analyse")
    source.add_argument("--device", help="serial of a connected phone (live screenshot)")
    parser.add_argument("--what", choices=("skip", "close", "all"), default="skip",
                        help="which button to look for (default: skip)")
    parser.add_argument("--threshold", type=float, default=DEFAULT_THRESHOLD,
                        help=f"minimal match score (default: {DEFAULT_THRESHOLD})")
    parser.add_argument("--region", choices=("right", "top-right", "bottom-right", "any"),
                        default="right",
                        help="where the button may be (default: right, above the banner)")
    parser.add_argument("--min-scale", type=float, default=DEFAULT_MIN_SCALE)
    parser.add_argument("--max-scale", type=float, default=DEFAULT_MAX_SCALE)
    parser.add_argument("--scale-step", type=float, default=DEFAULT_SCALE_STEP)
    parser.add_argument("--save", type=Path, help="write an annotated copy of the screen here")
    parser.add_argument("--show", action="store_true", help="open the annotated screen in a window")
    parser.add_argument("--adb", help="adb.exe to use (default: bundled platform-tools/adb.exe)")
    parser.add_argument("--tap", action="store_true",
                        help="tap the best match (needs a device)")
    parser.add_argument("--json", action="store_true", help="machine-readable output")
    parser.add_argument("--selftest", action="store_true", help="run the synthetic self-test")
    parser.add_argument("--verbose", "-v", action="store_true", help="print every match")
    args = parser.parse_args(argv)

    if args.selftest:
        return selftest(args.threshold)

    scales = parse_scales(args)
    templates = load_templates(args.what)

    adb = None
    serial = args.device
    if args.image is not None:
        image = read_image(args.image, cv2.IMREAD_COLOR)
        if image is None:
            raise SystemExit(f"cannot read {args.image}")
    else:
        adb = find_adb(args.adb)
        if not serial:
            devices = adb_devices(adb)
            if len(devices) != 1:
                raise SystemExit(
                    "choose a device with --device <serial> (connected: "
                    + (", ".join(devices) or "none") + ")")
            serial = devices[0]
        image = take_screenshot(adb, serial)

    started = time.perf_counter()
    detections = find_buttons(to_gray(image), templates, args.threshold, scales, args.region)
    elapsed_ms = (time.perf_counter() - started) * 1000.0

    if not detections:
        print(f"no {args.what} button found (threshold {args.threshold:.2f}, "
              f"region {args.region}, {elapsed_ms:.0f} ms)")
        if args.save is not None:
            save_image(args.save, annotate(image, []))
            print(f"annotated screen: {args.save}")
        if args.show:
            show_image(image, [])
        return 2

    if args.json:
        payload = {
            "image": str(args.image) if args.image else f"device:{serial}",
            "what": args.what,
            "threshold": args.threshold,
            "region": args.region,
            "elapsed_ms": round(elapsed_ms, 1),
            "detections": [det.as_dict() for det in detections],
        }
        print(json.dumps(payload, ensure_ascii=False, indent=2))
    else:
        print(f"{len(detections)} match(es) in {elapsed_ms:.0f} ms "
              f"(threshold {args.threshold:.2f}, region {args.region}):")
        for det in (detections if args.verbose else detections[:3]):
            print(f"  {det.kind:6s} x={det.x:4d} y={det.y:4d} "
                  f"score={det.score:.3f} scale={det.scale:.2f} "
                  f"template={det.template}")

    best = detections[0]
    if args.save is not None:
        save_image(args.save, annotate(image, detections))
        print(f"annotated screen: {args.save}")
    elif args.device:
        default_save = SCRIPT_DIR / "out" / f"{serial}_{int(time.time())}_marked.png"
        save_image(default_save, annotate(image, detections))
        print(f"annotated screen: {default_save}")

    if args.show:
        show_image(image, detections)

    if args.tap:
        if adb is None or not serial:
            raise SystemExit("--tap needs --device (a live phone)")
        print(f"tapping {best.kind} at ({best.x}, {best.y}) on {serial}")
        tap(adb, serial, best.x, best.y)
    return 0


if __name__ == "__main__":
    try:
        sys.stdout.reconfigure(encoding="utf-8")
    except Exception:
        pass
    raise SystemExit(main(sys.argv[1:]))
