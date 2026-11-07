@echo off
REM Double-click to try SmartSplint Monitor without the splint (simulated data, 3 days of history).
cd /d "%~dp0"
python server.py --simulate --demo-days 3
if errorlevel 1 (
  echo.
  echo Could not start. Make sure Python 3 is installed: https://www.python.org/downloads/
  pause
)
