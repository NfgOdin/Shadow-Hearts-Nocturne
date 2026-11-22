@echo off
setlocal EnableExtensions EnableDelayedExpansion
title Shadow Hearts: Nocturne - Trilogy Setup ^& Command Manager

set "ROOT=%~dp0"
set "CLI_MODE=0"

:: Direct CLI subcommand support (used by Shadow Hearts Nocturne.exe Tools tab & scripting):
::   "Shadow Hearts Nocturne Setup.cmd" build-koudelka | build-sh1 | build-sh2 | build-sh3 | build-all
::   "Shadow Hearts Nocturne Setup.cmd" play-launcher | play-koudelka | play-sh1 | play-sh2 | play-sh3
::   "Shadow Hearts Nocturne Setup.cmd" bake-vu1 | extract-sh1 | extract-sh2 | toolchain
if not "%~1"=="" (
    set "CLI_MODE=1"
    set "ACTION=%~1"
    shift
    goto :dispatch_cli
)

:menu
cls
call :detect_status
echo ========================================================================
echo   SHADOW HEARTS: NOCTURNE  -  Trilogy Setup ^& Command Manager
echo ========================================================================
echo   Launcher : !ST_LNCH!  Shadow Hearts Nocturne.exe
echo   Prequel  : !ST_KDL!  Koudelka (1898, PS1, 4 Discs)      [SLUS-01051..02]
echo   Game I   : !ST_SH1!  Shadow Hearts (1913)                 [SLUS-20347]
echo   Game II  : !ST_SH2!  Shadow Hearts: Covenant (1915)    [SLUS-21041/44]
echo   Game III : !ST_SH3!  Shadow Hearts: From the New World    [SLUS-21326]
echo ------------------------------------------------------------------------
echo   BUILD / UPDATE FROM YOUR OWN DISC IMAGES:
echo     [K] Stage / Build Koudelka (1898, PS1 4-Disc Prequel)
echo     [1] Build / Update Shadow Hearts (1913)
echo     [2] Build / Update Shadow Hearts: Covenant (1915)
echo     [3] Build / Update Shadow Hearts: From the New World (1929)
echo     [4] Build / Update ALL Three Games in Sequence
echo.
echo   PLAY / LAUNCH:
echo     [5] Open Collection Launcher (Shadow Hearts Nocturne.exe)
echo     [P] Play Koudelka (1898, PS1 4-Disc Prequel) Directly
echo     [6] Play Shadow Hearts (1913) Directly
echo     [7] Play Shadow Hearts: Covenant (1915) Directly
echo     [8] Play Shadow Hearts: From the New World (1929) Directly
echo.
echo   TOOLS ^& MAINTENANCE:
echo     [9] Bake Captured VU1 Microprograms into EXE (Zero JIT Stutter)
echo     [A] Extract Disc Textures / FMVs for HD Packs ^& AI Upscaling
echo     [T] Install / Repair Portable Build Toolchain Only
echo     [0] Exit
echo ========================================================================
echo.
set "CHOICE="
set /p "CHOICE=Select an option [0-9, K, P, A, T]: "
if not defined CHOICE goto :menu

if /i "%CHOICE%"=="0" exit /b 0
if /i "%CHOICE%"=="Q" exit /b 0
if /i "%CHOICE%"=="K" (call :do_build_koudelka & pause & goto :menu)
if /i "%CHOICE%"=="P" (call :do_play_koudelka & goto :menu)
if "%CHOICE%"=="1" (call :do_build_sh1 & pause & goto :menu)
if "%CHOICE%"=="2" (call :do_build_sh2 & pause & goto :menu)
if "%CHOICE%"=="3" (call :do_build_sh3 & pause & goto :menu)
if "%CHOICE%"=="4" (call :do_build_all & pause & goto :menu)
if "%CHOICE%"=="5" (call :do_play_launcher & goto :menu)
if "%CHOICE%"=="6" (call :do_play_sh1 & goto :menu)
if "%CHOICE%"=="7" (call :do_play_sh2 & goto :menu)
if "%CHOICE%"=="8" (call :do_play_sh3 & goto :menu)
if "%CHOICE%"=="9" (call :menu_bake_vu1 & goto :menu)
if /i "%CHOICE%"=="A" (call :menu_extract_pkb & goto :menu)
if /i "%CHOICE%"=="T" (call :ensure_toolchain_force & pause & goto :menu)

echo.
echo Invalid selection "%CHOICE%". Please choose from the menu above.
timeout /t 2 >nul
goto :menu

:dispatch_cli
set "EXTRA_ARGS="
:collect_args
if "%~1"=="" goto :run_cli
set "EXTRA_ARGS=!EXTRA_ARGS! "%~1""
shift
goto :collect_args

:run_cli
if /i "%ACTION%"=="build-koudelka" (call :do_build_koudelka !EXTRA_ARGS! & goto :cli_done)
if /i "%ACTION%"=="build-sh1" (call :do_build_sh1 !EXTRA_ARGS! & goto :cli_done)
if /i "%ACTION%"=="build-sh2" (call :do_build_sh2 !EXTRA_ARGS! & goto :cli_done)
if /i "%ACTION%"=="build-sh3" (call :do_build_sh3 !EXTRA_ARGS! & goto :cli_done)
if /i "%ACTION%"=="build-all" (call :do_build_all & goto :cli_done)
if /i "%ACTION%"=="play-launcher" (call :do_play_launcher & exit /b !ERRORLEVEL!)
if /i "%ACTION%"=="play-koudelka" (call :do_play_koudelka !EXTRA_ARGS! & exit /b !ERRORLEVEL!)
if /i "%ACTION%"=="play-sh1" (call :do_play_sh1 !EXTRA_ARGS! & exit /b !ERRORLEVEL!)
if /i "%ACTION%"=="play-sh2" (call :do_play_sh2 !EXTRA_ARGS! & exit /b !ERRORLEVEL!)
if /i "%ACTION%"=="play-sh3" (call :do_play_sh3 !EXTRA_ARGS! & exit /b !ERRORLEVEL!)
if /i "%ACTION%"=="bake-vu1" (call :menu_bake_vu1 & goto :cli_done)
if /i "%ACTION%"=="toolchain" (call :ensure_toolchain_force & goto :cli_done)
echo Unknown command: "%ACTION%"
echo Usage: "%~nx0" [build-koudelka^|build-sh1^|build-sh2^|build-sh3^|build-all^|play-launcher^|play-koudelka^|play-sh1^|play-sh2^|play-sh3^|bake-vu1^|toolchain]
pause
exit /b 2

:cli_done
set "CLI_ERR=%ERRORLEVEL%"
echo.
if not "%CLI_ERR%"=="0" echo Command "%ACTION%" finished with exit code %CLI_ERR%.
pause
exit /b %CLI_ERR%

:: ========================================================================
:: Status Detection
:: ========================================================================
:detect_status
set "ST_LNCH=[NOT BUILT]"
set "ST_KDL=[NOT STAGED]"
set "ST_SH1=[NOT BUILT]"
set "ST_SH2=[NOT BUILT]"
set "ST_SH3=[NOT BUILT]"
if exist "%ROOT%Shadow Hearts Nocturne.exe" set "ST_LNCH=[READY]    "
if exist "%ROOT%games\koudelka\disc_files\disc_manifest.tsv" set "ST_KDL=[4 DISCS]  "
if exist "%ROOT%games\koudelka\Koudelka.exe" set "ST_KDL=[READY]    "
if exist "%ROOT%games\sh1\SH1.exe" set "ST_SH1=[READY]    "
if exist "%ROOT%games\sh2-covenant\SH2C.exe" set "ST_SH2=[READY]    "
if exist "%ROOT%games\sh3-ftnw\SH3.exe" set "ST_SH3=[READY]    "
exit /b 0

:: ========================================================================
:: Portable Toolchain Bootstrap
:: ========================================================================
:ensure_toolchain
"%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe" -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%ROOT%scripts\portable.ps1" -Check >nul 2>&1
if errorlevel 1 (
    echo.
    echo Portable toolchain missing or incomplete. Downloading/repairing tools into tools\portable...
    echo.
    "%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe" -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%ROOT%scripts\portable.ps1"
    if errorlevel 1 (
        echo.
        echo Failed to download portable toolchain.
        exit /b 1
    )
)
set "PATH=%ROOT%tools\portable\llvm-mingw\bin;%ROOT%tools\portable\cmake\bin;%ROOT%tools\portable\ninja;%ROOT%tools\portable\python;%ROOT%tools\portable\mingit\cmd;%ROOT%tools\portable\mingit\usr\bin;%ROOT%tools\portable\busybox;%PATH%"
exit /b 0

:ensure_toolchain_force
echo.
echo Checking / repairing portable toolchain in tools\portable...
"%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe" -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%ROOT%scripts\portable.ps1"
exit /b %ERRORLEVEL%

:: ========================================================================
:: Build Targets
:: ========================================================================
:do_build_koudelka
call :ensure_toolchain || exit /b 1
echo.
"%ROOT%tools\portable\python\python.exe" -u "%ROOT%scripts\koudelka_build.py" %*
set "BERR=%ERRORLEVEL%"
if not "%BERR%"=="0" echo Koudelka setup failed with exit code %BERR%.
exit /b %BERR%

:do_build_sh1
call :ensure_toolchain || exit /b 1
echo.
"%ROOT%tools\portable\python\python.exe" -u "%ROOT%scripts\sh1_build.py" %*
set "BERR=%ERRORLEVEL%"
if not "%BERR%"=="0" echo Build failed with exit code %BERR%.
exit /b %BERR%

:do_build_sh2
call :ensure_toolchain || exit /b 1
echo.
"%ROOT%tools\portable\python\python.exe" -u "%ROOT%scripts\sh2_build.py" %*
set "BERR=%ERRORLEVEL%"
if not "%BERR%"=="0" echo Build failed with exit code %BERR%.
exit /b %BERR%

:do_build_sh3
call :ensure_toolchain || exit /b 1
echo.
"%ROOT%tools\portable\python\python.exe" -u "%ROOT%scripts\sh3_build.py" %*
set "BERR=%ERRORLEVEL%"
if not "%BERR%"=="0" echo Build failed with exit code %BERR%.
exit /b %BERR%

:do_build_all
call :ensure_toolchain || exit /b 1
echo.
echo [1/3] Building Shadow Hearts: Covenant (1915) + Nocturne Launcher...
"%ROOT%tools\portable\python\python.exe" -u "%ROOT%scripts\sh2_build.py"
if errorlevel 1 exit /b %ERRORLEVEL%
echo.
echo [2/3] Building Shadow Hearts (1913)...
"%ROOT%tools\portable\python\python.exe" -u "%ROOT%scripts\sh1_build.py"
if errorlevel 1 exit /b %ERRORLEVEL%
echo.
echo [3/3] Building Shadow Hearts: From the New World (1929)...
"%ROOT%tools\portable\python\python.exe" -u "%ROOT%scripts\sh3_build.py"
exit /b %ERRORLEVEL%

:: ========================================================================
:: Direct Launch Targets
:: ========================================================================
:do_play_launcher
if not exist "%ROOT%Shadow Hearts Nocturne.exe" (
    echo.
    echo Shadow Hearts Nocturne.exe has not been built yet.
    echo Please build at least one game from options [K], [1]-[3] first.
    pause
    exit /b 1
)
start "" "%ROOT%Shadow Hearts Nocturne.exe"
exit /b 0

:do_play_koudelka
set "PATH=%ROOT%;%ROOT%tools\portable\llvm-mingw\bin;%PATH%"
set "GAME_DIR=%ROOT%games\koudelka"
set "EXE=%GAME_DIR%\Koudelka.exe"
if not exist "%EXE%" set "EXE=%ROOT%build\native-koudelka\psxrun.exe"
if not exist "%EXE%" (
    echo.
    echo Koudelka (1898) has not been built yet. Choose option [K] first.
    pause
    exit /b 1
)
start "" "%EXE%" "%GAME_DIR%" %*
exit /b 0

:do_play_sh1
set "PATH=%ROOT%;%ROOT%tools\portable\llvm-mingw\bin;%PATH%"
set "PS2_WINDOW_TITLE=Shadow Hearts"
set "PS2_SCREENSHOTS_DIR=%USERPROFILE%\Pictures\Shadow Hearts Nocturne\Shadow Hearts"
set "PS2_SCREENSHOT_PREFIX=ShadowHearts"
set "GAME_DIR=%ROOT%games\sh1"
set "EXE=%GAME_DIR%\SH1.exe"
if not exist "%EXE%" set "EXE=%ROOT%build\native-sh1\ps2run.exe"
if not exist "%EXE%" (
    echo.
    echo Shadow Hearts (1913) has not been built yet. Choose option [1] first.
    pause
    exit /b 1
)
set "DISC1=%GAME_DIR%\disc_files"
if not exist "%DISC1%\disc_manifest.tsv" set "DISC1=%GAME_DIR%\disc.iso"
start "" "%EXE%" --disc "%DISC1%" --game-dir "%GAME_DIR%" --memcard "%GAME_DIR%\memcard.ps2" --window %*
exit /b 0

:do_play_sh2
set "PATH=%ROOT%;%ROOT%tools\portable\llvm-mingw\bin;%PATH%"
set "PS2_WINDOW_TITLE=Shadow Hearts: Covenant"
set "PS2_SCREENSHOTS_DIR=%USERPROFILE%\Pictures\Shadow Hearts Nocturne\Covenant"
set "PS2_SCREENSHOT_PREFIX=ShadowHeartsCovenant"
set "GAME_DIR=%ROOT%games\sh2-covenant"
set "EXE=%GAME_DIR%\SH2C.exe"
if not exist "%EXE%" set "EXE=%ROOT%build\native-sh2-covenant\ps2run.exe"
if not exist "%EXE%" (
    echo.
    echo Shadow Hearts: Covenant has not been built yet. Choose option [2] first.
    pause
    exit /b 1
)
set "DISC1=%GAME_DIR%\disc1_files"
if not exist "%DISC1%\disc_manifest.tsv" set "DISC1=%GAME_DIR%\disc.iso"
set "DISC2="
if exist "%GAME_DIR%\disc2_files\disc_manifest.tsv" set "DISC2=%GAME_DIR%\disc2_files"
if not defined DISC2 if exist "%GAME_DIR%\disc2.iso" set "DISC2=%GAME_DIR%\disc2.iso"
if defined DISC2 (
    start "" "%EXE%" --disc "%DISC1%" --disc2 "%DISC2%" --game-dir "%GAME_DIR%" --memcard "%GAME_DIR%\memcard.ps2" --window %*
) else (
    start "" "%EXE%" --disc "%DISC1%" --game-dir "%GAME_DIR%" --memcard "%GAME_DIR%\memcard.ps2" --window %*
)
exit /b 0

:do_play_sh3
set "PATH=%ROOT%;%ROOT%tools\portable\llvm-mingw\bin;%PATH%"
set "PS2_WINDOW_TITLE=Shadow Hearts: From the New World"
set "PS2_SCREENSHOTS_DIR=%USERPROFILE%\Pictures\Shadow Hearts Nocturne\From the New World"
set "PS2_SCREENSHOT_PREFIX=ShadowHeartsNewWorld"
set "GAME_DIR=%ROOT%games\sh3-ftnw"
set "EXE=%GAME_DIR%\SH3.exe"
if not exist "%EXE%" set "EXE=%ROOT%build\native-sh3-ftnw\ps2run.exe"
if not exist "%EXE%" (
    echo.
    echo Shadow Hearts: From the New World has not been built yet. Choose option [3] first.
    pause
    exit /b 1
)
set "DISC1=%GAME_DIR%\disc_files"
if not exist "%DISC1%\disc_manifest.tsv" set "DISC1=%GAME_DIR%\disc.iso"
start "" "%EXE%" --disc "%DISC1%" --game-dir "%GAME_DIR%" --memcard "%GAME_DIR%\memcard.ps2" --window %*
exit /b 0

:: ========================================================================
:: Tools Submenus (VU1 AOT Baker & PKB Texture / FMV Extractor)
:: ========================================================================
:menu_bake_vu1
call :ensure_toolchain || exit /b 1
echo.
echo   --- Bake Captured VU1/VU0 Microprograms into EXE ---
echo     [1] Shadow Hearts (1913)
echo     [2] Shadow Hearts: Covenant (1915)
echo     [3] Shadow Hearts: From the New World (1929)
echo     [0] Back
echo.
set "VU_CH="
set /p "VU_CH=Select game [0-3]: "
if "%VU_CH%"=="1" ("%ROOT%tools\portable\python\python.exe" -u "%ROOT%scripts\sh1_precompile_vu1.py" & pause)
if "%VU_CH%"=="2" ("%ROOT%tools\portable\python\python.exe" -u "%ROOT%scripts\sh2_precompile_vu1.py" & pause)
if "%VU_CH%"=="3" ("%ROOT%tools\portable\python\python.exe" -u "%ROOT%scripts\sh2_precompile_vu1.py" --game sh3 & pause)
exit /b 0

:menu_extract_pkb
call :ensure_toolchain || exit /b 1
echo.
echo   --- Offline Disc Texture ^& FMV Extraction / AI Upscaling ---
echo     [1] Shadow Hearts (1913)    - Dump All Textures (PNG)
echo     [2] Shadow Hearts (1913)    - Dump + 4x AI Upscale Textures
echo     [3] Shadow Hearts (1913)    - Dump All 19 FMVs (MP4)
echo     [4] Shadow Hearts: Covenant - Dump All Textures (PNG)
echo     [5] Shadow Hearts: Covenant - Dump + 4x AI Upscale Textures
echo     [6] Shadow Hearts: Covenant - Dump All FMVs (MP4)
echo     [0] Back
echo.
set "PK_CH="
set /p "PK_CH=Select operation [0-6]: "
if "%PK_CH%"=="1" ("%ROOT%tools\portable\python\python.exe" -u "%ROOT%scripts\sh1_extract_pkb.py" --disc "%ROOT%games\sh1\disc_files\layer0" --extract-tex "%ROOT%games\sh1\dumps\textures" --clean & pause)
if "%PK_CH%"=="2" ("%ROOT%tools\portable\python\python.exe" -u "%ROOT%scripts\sh1_extract_pkb.py" --disc "%ROOT%games\sh1\disc_files\layer0" --extract-tex "%ROOT%games\sh1\dumps\textures" --clean --upscale --output "%ROOT%games\sh1\mods\textures\SH1_HD_Pack" --scale 4 & pause)
if "%PK_CH%"=="3" ("%ROOT%tools\portable\python\python.exe" -u "%ROOT%scripts\sh1_extract_pkb.py" --disc "%ROOT%games\sh1\disc_files\layer0" --extract-fmv "%ROOT%games\sh1\dumps\fmv" --clean & pause)
if "%PK_CH%"=="4" ("%ROOT%tools\portable\python\python.exe" -u "%ROOT%scripts\sh2_extract_pkb.py" --disc "%ROOT%games\sh2-covenant\disc1_files\layer0" --extract-tex "%ROOT%games\sh2-covenant\dumps\textures" --clean & pause)
if "%PK_CH%"=="5" ("%ROOT%tools\portable\python\python.exe" -u "%ROOT%scripts\sh2_extract_pkb.py" --disc "%ROOT%games\sh2-covenant\disc1_files\layer0" --extract-tex "%ROOT%games\sh2-covenant\dumps\textures" --clean --upscale --output "%ROOT%games\sh2-covenant\mods\textures\SH2_HD_Pack" --scale 4 & pause)
if "%PK_CH%"=="6" ("%ROOT%tools\portable\python\python.exe" -u "%ROOT%scripts\sh2_extract_pkb.py" --disc "%ROOT%games\sh2-covenant\disc1_files\layer0" --extract-fmv "%ROOT%games\sh2-covenant\dumps\fmv" --clean & pause)
exit /b 0
