@echo off
setlocal
set "ROOT=%~dp0"
if "%ROOT:~-1%"=="\" set "ROOT=%ROOT:~0,-1%"
if exist "%ROOT%\tools\portable\python\python.exe" (
    start "" /b "%ROOT%\tools\portable\python\python.exe" "%ROOT%\scripts\nocturne_mod_studio.py" --install-dir "%ROOT%" %*
) else (
    start "" /b python "%ROOT%\scripts\nocturne_mod_studio.py" --install-dir "%ROOT%" %*
)
