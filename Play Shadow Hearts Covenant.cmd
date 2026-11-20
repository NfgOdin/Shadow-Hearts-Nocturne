@echo off
setlocal
set "ROOT=%~dp0"
set "PATH=%ROOT%tools\portable\llvm-mingw\bin;%PATH%"
set "PS2_WINDOW_TITLE=Shadow Hearts: Nocturne"
set "GAME_DIR=%ROOT%games\sh2-covenant"
set "EXE=%ROOT%Shadow Hearts Nocturne.exe"
if not exist "%EXE%" set "EXE=%ROOT%build\native-sh2-covenant\ps2run.exe"
if not exist "%EXE%" (
    echo Shadow Hearts Nocturne has not been built yet.
    echo Please run "Build Shadow Hearts Covenant.cmd" first.
    pause
    exit /b 1
)
set "DISC1=%GAME_DIR%\disc1_files"
if not exist "%DISC1%\disc_manifest.tsv" set "DISC1=%GAME_DIR%\disc.iso"
set "DISC2="
if exist "%GAME_DIR%\disc2_files\disc_manifest.tsv" set "DISC2=%GAME_DIR%\disc2_files"
if not defined DISC2 if exist "%GAME_DIR%\disc2.iso" set "DISC2=%GAME_DIR%\disc2.iso"
if defined DISC2 (
    "%EXE%" --disc "%DISC1%" --disc2 "%DISC2%" --game-dir "%GAME_DIR%" --memcard "%GAME_DIR%\memcard.ps2" --window %*
) else (
    "%EXE%" --disc "%DISC1%" --game-dir "%GAME_DIR%" --memcard "%GAME_DIR%\memcard.ps2" --window %*
)

