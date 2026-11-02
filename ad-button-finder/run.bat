@echo off
rem Standalone ad button finder (OpenCV).
rem   run.bat --device EIPBNFCU6PRWKF5T
rem   run.bat --image shot.png --save shot.marked.png
rem   run.bat --selftest
cd /d "%~dp0"
python "%~dp0find_ad_buttons.py" %*
if errorlevel 2 pause
