import os
import shutil
import subprocess
import sys
import time

import cv2
import numpy as np

BUTTONS_FOLDER = "buttons"          # button samples (crops), any size
SCREENSHOTS_FOLDER = "screenshots"  # screenshots for --test mode
THRESHOLD = 0.85                    # minimal match score for full-screen search
THRESHOLD_ZONE = 0.68               # lower threshold for right-side zone only
RIGHT_ZONE = 0.75                   # x fraction: right portion where skip/close buttons live
WORK_WIDTH = 540                    # screenshots are downscaled to this width for speed
SCALES = np.linspace(0.4, 2.5, 22)  # template sizes to try
CHECK_INTERVAL = 1.0                # seconds between screenshots
TAP_COOLDOWN = 2.0                  # seconds to wait after a tap on the same device

IMAGE_EXTENSIONS = (".png", ".jpg", ".jpeg", ".bmp", ".webp")

def read_image(path, flags=cv2.IMREAD_COLOR):
	# cv2.imread can't open paths with Cyrillic letters on Windows
	return cv2.imdecode(np.fromfile(path, np.uint8), flags)

def find_adb():
	adb = shutil.which("adb")
	if adb:
		return adb
	default = os.path.expandvars(r"%LOCALAPPDATA%\Android\Sdk\platform-tools\adb.exe")
	if os.path.isfile(default):
		return default
	sys.exit("adb не найден. Установите Android platform-tools или добавьте adb в PATH.")

def load_templates(folder=BUTTONS_FOLDER):
	templates = []
	for name in sorted(os.listdir(folder)):
		if name.lower().endswith(IMAGE_EXTENSIONS):
			image = read_image(os.path.join(folder, name), cv2.IMREAD_GRAYSCALE)
			if image is not None:
				templates.append((name, image))
	if not templates:
		sys.exit(f"В папке {folder} нет образцов кнопки.")
	return templates

def find_button(screenshot, templates):
	# returns (x, y, score) in screenshot coordinates, or None
	factor = WORK_WIDTH / screenshot.shape[1]
	gray = cv2.cvtColor(screenshot, cv2.COLOR_BGR2GRAY)
	gray = cv2.resize(gray, None, fx=factor, fy=factor, interpolation=cv2.INTER_AREA)
	sh, sw = gray.shape

	# Right zone: skip/close buttons always live here
	x_zone = int(sw * RIGHT_ZONE)
	gray_zone = gray[:, x_zone:]

	best = None
	for _, template in templates:
		for scale in SCALES:
			resized = cv2.resize(template, None, fx=scale, fy=scale)
			h, w = resized.shape
			if min(h, w) < 5:
				continue

			# Full-screen search at high threshold
			if h <= sh and w <= sw:
				result = cv2.matchTemplate(gray, resized, cv2.TM_CCOEFF_NORMED)
				_, score, _, (x, y) = cv2.minMaxLoc(result)
				if score >= THRESHOLD:
					cx = (x + w / 2) / factor
					cy = (y + h / 2) / factor
					if best is None or score > best[2]:
						best = (cx, cy, score)

			# Right-zone search at lower threshold (catches style variations)
			if h <= gray_zone.shape[0] and w <= gray_zone.shape[1]:
				result = cv2.matchTemplate(gray_zone, resized, cv2.TM_CCOEFF_NORMED)
				_, score, _, (x, y) = cv2.minMaxLoc(result)
				if score >= THRESHOLD_ZONE:
					cx = (x_zone + x + w / 2) / factor
					cy = (y + h / 2) / factor
					if best is None or score > best[2]:
						best = (cx, cy, score)

	if best is None:
		return None
	return int(best[0]), int(best[1]), best[2]

def get_devices(adb):
	output = subprocess.run([adb, "devices"], capture_output=True, text=True).stdout
	return [line.split()[0] for line in output.splitlines()[1:] if line.endswith("\tdevice")]

def take_screenshot(adb, device):
	data = subprocess.run([adb, "-s", device, "exec-out", "screencap", "-p"], capture_output=True).stdout
	if not data:
		return None
	return cv2.imdecode(np.frombuffer(data, np.uint8), cv2.IMREAD_COLOR)

def tap(adb, device, x, y):
	subprocess.run([adb, "-s", device, "shell", "input", "tap", str(x), str(y)])

def run_on_devices(templates):
	adb = find_adb()
	last_tap = {}
	print("Слежу за экранами телефонов. Остановить: Ctrl+C")

	while True:
		devices = get_devices(adb)
		if not devices:
			print("Телефоны не подключены, жду...")

		for device in devices:
			if time.time() - last_tap.get(device, 0) < TAP_COOLDOWN:
				continue

			screenshot = take_screenshot(adb, device)
			if screenshot is None:
				continue

			found = find_button(screenshot, templates)
			if found:
				x, y, score = found
				tap(adb, device, x, y)
				last_tap[device] = time.time()
				print(f"[{time.strftime('%H:%M:%S')}] {device}: нажал ({x}, {y}), совпадение {score:.2f}")

		time.sleep(CHECK_INTERVAL)

def run_test(templates):
	# finds the button on screenshots from the folder and saves marked copies to screenshots/result
	result_folder = os.path.join(SCREENSHOTS_FOLDER, "result")
	os.makedirs(result_folder, exist_ok=True)

	for name in sorted(os.listdir(SCREENSHOTS_FOLDER)):
		if not name.lower().endswith(IMAGE_EXTENSIONS):
			continue
		screenshot = read_image(os.path.join(SCREENSHOTS_FOLDER, name))
		if screenshot is None:
			continue

		found = find_button(screenshot, templates)
		if found:
			x, y, score = found
			radius = max(10, screenshot.shape[1] // 25)
			cv2.circle(screenshot, (x, y), radius, (0, 0, 255), 3)
			print(f"{name}: кнопка в ({x}, {y}), совпадение {score:.2f}")
		else:
			print(f"{name}: кнопка не найдена")
		cv2.imencode(os.path.splitext(name)[1], screenshot)[1].tofile(os.path.join(result_folder, name))

	print(f"Отмеченные скриншоты сохранены в {result_folder}")

if __name__ == "__main__":
	os.chdir(os.path.dirname(os.path.abspath(__file__)))
	templates = load_templates()

	if "--test" in sys.argv:
		run_test(templates)
	else:
		run_on_devices(templates)
