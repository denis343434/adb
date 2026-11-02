# Ad button finder (OpenCV)

A **standalone** tool that finds the **Skip** button of a full-screen ad on a
phone screenshot and prints its coordinates.  It is a separate program from
the device-farm player: nothing in the player calls it, and it does not play
the game.

`close` support is already wired in (`--what close`); it only needs a
`templates/close.png` crop.

## What is in the folder

```
find_ad_buttons.py     the detector
templates/             button crops: skip*.png, close*.png
platform-tools/        adb.exe + its DLLs (bundled, no system ADB needed)
out/                   annotated screenshots from --device (created on use)
```

## Quick start

```powershell
# analyse a screenshot you already have
python find_ad_buttons.py --image ad.png --save ad.marked.png

# take a fresh screenshot from the phone and annotate it
python find_ad_buttons.py --device EIPBNFCU6PRWKF5T

# open the annotated screenshot in a window
python find_ad_buttons.py --image ad.png --show

# machine-readable output (for wiring into another tool later)
python find_ad_buttons.py --image ad.png --json

# self-test on a synthetic screen (no phone needed)
python find_ad_buttons.py --selftest
```

Exit code is `0` when something was found, `2` when nothing matched.
When `--device` is used, the annotated screenshot goes to `out/`.

`--tap` presses the best match with `adb shell input tap` (only with a
device).  It is off by default on purpose.

## How it works

1. Templates are read from `templates/`; a file name decides the kind:
   `skip*.png` -> Skip, `close*.png` -> Close.
2. A template is auto-cropped to the icon (flat background removed with a
   median border colour).  Without that, a mostly-white crop is dominated by
   white in `TM_CCOEFF_NORMED` and the real button scores poorly.
3. The icon is matched at many scales (1.0 … 2.6 by default), so the same crop
   works when an ad draws the button larger or smaller.
4. Matches are kept on the right side of the screen (ad controls live there)
   and above the permanent in-game banner at the bottom, then duplicates from
   adjacent scales are merged and the best score wins.

## Tuning

| Flag | Default | Meaning |
| --- | --- | --- |
| `--threshold` | `0.85` | Minimal score.  Raise it if a game screen produces a false positive. |
| `--region` | `right` | `top-right`, `bottom-right`, `right`, `any`. |
| `--min-scale` / `--max-scale` / `--scale-step` | `1.0` / `2.6` / `0.05` | Icon size range relative to the template. |
| `--adb` | bundled `platform-tools/adb.exe` | Use a different adb. |
| `--show` | off | Open the annotated screen in a window. |
| `--verbose` | off | Print every cluster, not just the top three. |
| `--json` | off | JSON instead of text. |

## Notes

* `TM_CCOEFF_NORMED` is very good at the exact icon, but the Skip glyph is
  generic (a triangle plus a bar).  The region filter and the bottom-banner
  cutoff do most of the work against false positives; if a new ad layout
  produces one, send the screenshot and the threshold/region can be tuned.
* The template in `templates/skip.png` is the light (grey-on-white) crop;
  `templates/skip_dark.png` is the dark (white-on-black) variant.  Every
  `skip*.png` in `templates/` is tried on every run, so a new ad theme only
  needs a new crop file — no code change.
* `--selftest` builds a synthetic screen from each template's own background
  colour and checks that the right template wins at the right place; it also
  checks that a blank screen produces no match.
* Verified on a real 720x1600 light ad (`aeclinicmed.ru`): Skip found at
  `(670, 128)`, score `0.929`, scale `2.20`; a synthetic dark ad produced
  `(626, 112)` score `0.996` at scale `1.60` and `(575, 1311)` score `0.993` at
  scale `2.20`; six non-ad game screens produced no matches at the default
  threshold.
