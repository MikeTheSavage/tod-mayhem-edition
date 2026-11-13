@echo off
rem Gameplay Plus Installer.cmd - the visual installer for TOD Gameplay+ (installer\README.md).
rem Opens a window (Microsoft Edge app mode, or the default browser); this console closes at once. Options: installerpp.py --help
setlocal
set ROOT=%~dp0
set PY=
for %%P in ("%LOCALAPPDATA%\Programs\Python\Python312\pythonw.exe" "%LOCALAPPDATA%\Programs\Python\Python312\python.exe") do (
  if not defined PY if exist %%P set PY="%%~P"
)
if not defined PY (
  where py >nul 2>nul && set PY=py -3
)
if not defined PY (
  echo Python 3 was not found. Install Python 3.12 from python.org, then run this again.
  pause
  exit /b 1
)
start "" %PY% -I "%ROOT%installer\app.py" %*
