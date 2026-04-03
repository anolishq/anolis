@echo off
cd /d "%~dp0\..\.."
echo Starting Anolis System Composer...
where python >nul 2>&1
if %ERRORLEVEL% equ 0 (
    python tools\system-composer\backend\server.py
) else (
    python3 tools\system-composer\backend\server.py
)
if %ERRORLEVEL% neq 0 (
    echo.
    echo ERROR: Failed to start. Check output above.
    pause
)
