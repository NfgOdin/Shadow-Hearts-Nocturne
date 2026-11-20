@echo off
setlocal
set "ROOT=%~dp0"
set "PATH=%ROOT%tools\portable\llvm-mingw\bin;%ROOT%tools\portable\cmake\bin;%ROOT%tools\portable\ninja;%ROOT%tools\portable\python;%PATH%"
"%ROOT%tools\portable\python\python.exe" -u "%ROOT%scripts\sh2_build.py" %*
set "ERR=%ERRORLEVEL%"
echo.
if not "%ERR%"=="0" (
    echo Build failed with exit code %ERR%.
)
pause
exit /b %ERR%
