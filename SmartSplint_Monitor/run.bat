@echo off
REM Double-click to start SmartSplint Monitor (connects to the real splint).
cd /d "%~dp0"
python server.py
if errorlevel 1 (
  echo.
  echo Could not start. Make sure Python 3 is installed: https://www.python.org/downloads/
  pause
)
