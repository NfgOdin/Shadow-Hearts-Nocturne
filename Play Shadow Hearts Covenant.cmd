@echo off
setlocal
set "ROOT=%~dp0"
set "PATH=%ROOT%tools\portable\llvm-mingw\bin;%PATH%"
set "PS2_WINDOW_TITLE=Shadow Hearts: Covenant"
"%ROOT%build\native-sh2-covenant\ps2run.exe" --disc "%ROOT%games\sh2-covenant\disc.iso" --disc2 "%ROOT%games\sh2-covenant\disc2.iso" --game-dir "%ROOT%games\sh2-covenant" --memcard "%ROOT%games\sh2-covenant\memcard.ps2" --window %*
