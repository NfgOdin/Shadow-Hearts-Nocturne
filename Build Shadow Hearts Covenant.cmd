@echo off
setlocal
set "ROOT=%~dp0"
if not exist "%ROOT%tools\portable\python\python.exe" (
    echo Portable toolchain not found. Downloading llvm-mingw, CMake, Ninja, and Python into tools\portable...
    echo.
    "%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe" -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%ROOT%scripts\portable.ps1"
    if errorlevel 1 (
        echo.
        echo Failed to download portable toolchain.
        pause
        exit /b 1
    )
)
set "PATH=%ROOT%tools\portable\llvm-mingw\bin;%ROOT%tools\portable\cmake\bin;%ROOT%tools\portable\ninja;%ROOT%tools\portable\python;%ROOT%tools\portable\mingit\cmd;%ROOT%tools\portable\mingit\usr\bin;%ROOT%tools\portable\busybox;%PATH%"
"%ROOT%tools\portable\python\python.exe" -u "%ROOT%scripts\sh2_build.py" %*
set "ERR=%ERRORLEVEL%"
echo.
if not "%ERR%"=="0" (
    echo Build failed with exit code %ERR%.
)
pause
exit /b %ERR%
