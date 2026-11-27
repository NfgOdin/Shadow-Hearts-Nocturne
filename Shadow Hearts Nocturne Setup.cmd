@echo off
setlocal EnableExtensions EnableDelayedExpansion
title Shadow Hearts: Nocturne - Collection Setup ^& Command Manager

set "ROOT=%~dp0"
set "INSTALL_DIR=%ROOT:~0,-1%"
set "CLI_MODE=0"

:: Direct CLI subcommand support (used by Shadow Hearts Nocturne.exe Tools tab & scripting):
::   "Shadow Hearts Nocturne Setup.cmd" wizard | update | check-update | build-koudelka | build-sh1 | build-sh2 | build-sh3 | build-all
::   "Shadow Hearts Nocturne Setup.cmd" play-launcher | play-koudelka | play-sh1 | play-sh2 | play-sh3
::   "Shadow Hearts Nocturne Setup.cmd" bake-vu1 | extract-sh1 | extract-sh2 | toolchain
if not "%~1"=="" (
    set "CLI_MODE=1"
    set "ACTION=%~1"
    shift
    goto :dispatch_cli
)

:: Interactive launch: bootstrap toolchain if needed, then auto-start the Step-by-Step
:: Setup Walkthrough on a first-time install (when no games have been built yet).
call :ensure_toolchain || (pause & exit /b 1)
call :refresh_install_dir
"%ROOT%tools\portable\python\python.exe" "%ROOT%scripts\nocturne_setup.py" --check-first-run >nul 2>&1
if "%ERRORLEVEL%"=="0" (
    call :do_wizard
    set "WERR=!ERRORLEVEL!"
    if "!WERR!"=="10" exit /b 0
    if "!WERR!"=="0" exit /b 0
    rem Exit code 2 means user chose [M] Advanced Command Menu
)

:menu
cls
call :refresh_install_dir
call :detect_status
echo ========================================================================
echo   SHADOW HEARTS: NOCTURNE  -  Collection Setup ^& Command Manager
echo ========================================================================
echo   Install  : !INSTALL_DIR!
echo   Launcher : !ST_LNCH!  Shadow Hearts Nocturne.exe
echo   Prequel  : !ST_KDL!  Koudelka (1898, PS1, 4 Discs)      [SLUS-01051..02]
echo   Game I   : !ST_SH1!  Shadow Hearts (1913)                 [SLUS-20347]
echo   Game II  : !ST_SH2!  Shadow Hearts: Covenant (1915)    [SLUS-21041/44]
echo   Game III : !ST_SH3!  Shadow Hearts: From the New World    [SLUS-21326]
echo ------------------------------------------------------------------------
echo   GUIDED INSTALLER ^& BUILD / UPDATE:
echo     [W] Step-by-Step Setup Walkthrough (All 4 Games, Folder, ROM Scan)
echo     [4] Build / Update ALL 4 Games in Sequence (Koudelka + SH1-SH3)
echo     [K] Build / Update Koudelka (1898, PS1 4-Disc Prequel)
echo     [1] Build / Update Shadow Hearts (1913)
echo     [2] Build / Update Shadow Hearts: Covenant (1915)
echo     [3] Build / Update Shadow Hearts: From the New World (1929)
echo.
echo   PLAY / LAUNCH:
echo     [5] Open Collection Launcher (Shadow Hearts Nocturne.exe)
echo     [P] Play Koudelka (1898, PS1 4-Disc Prequel) Directly
echo     [6] Play Shadow Hearts (1913) Directly
echo     [7] Play Shadow Hearts: Covenant (1915) Directly
echo     [8] Play Shadow Hearts: From the New World (1929) Directly
echo.
echo   TOOLS ^& MAINTENANCE:
echo     [U] Check for Updates / Switch Channel (Main / Nightly)
echo     [D] Upgrade Covenant to Director's Cut (English Dub / UnDub)
echo     [9] Bake Captured VU1 Microprograms into EXE (Zero JIT Stutter)
echo     [A] Extract Disc Textures / FMVs for HD Packs ^& AI Upscaling
echo     [T] Install / Repair Portable Build Toolchain Only
echo     [0] Exit
echo ========================================================================
echo.
set "CHOICE="
set /p "CHOICE=Select an option [W, U, 0-9, K, P, A, T]: "
if not defined CHOICE goto :menu

if /i "%CHOICE%"=="0" exit /b 0
if /i "%CHOICE%"=="Q" exit /b 0
if /i "%CHOICE%"=="W" (call :do_wizard & goto :after_wizard)
if /i "%CHOICE%"=="U" (call :do_update & goto :after_build)
if /i "%CHOICE%"=="D" (call :do_upgrade_sh2_dc & goto :after_build)
if /i "%CHOICE%"=="K" (call :do_build_koudelka & goto :after_build)
if /i "%CHOICE%"=="P" (call :do_play_koudelka & goto :menu)
if "%CHOICE%"=="1" (call :do_build_sh1 & goto :after_build)
if "%CHOICE%"=="2" (call :do_build_sh2 & goto :after_build)
if "%CHOICE%"=="3" (call :do_build_sh3 & goto :after_build)
if "%CHOICE%"=="4" (call :do_build_all & goto :after_build)
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

:after_wizard
if "%ERRORLEVEL%"=="10" exit /b 0
if "%ERRORLEVEL%"=="2" goto :menu
pause
goto :menu

:after_build
if "%ERRORLEVEL%"=="10" exit /b 0
pause
goto :menu

:dispatch_cli
set "EXTRA_ARGS="
:collect_args
if "%~1"=="" goto :run_cli
set "EXTRA_ARGS=!EXTRA_ARGS! "%~1""
shift
goto :collect_args

:run_cli
if /i "%ACTION%"=="wizard" (call :do_wizard !EXTRA_ARGS! & goto :cli_done)
if /i "%ACTION%"=="update" (call :do_update !EXTRA_ARGS! & goto :cli_done)
if /i "%ACTION%"=="check-update" (call :do_check_update !EXTRA_ARGS! & goto :cli_done)
if /i "%ACTION%"=="build-koudelka" (call :do_build_koudelka !EXTRA_ARGS! & goto :cli_done)
if /i "%ACTION%"=="build-sh1" (call :do_build_sh1 !EXTRA_ARGS! & goto :cli_done)
if /i "%ACTION%"=="build-sh2" (call :do_build_sh2 !EXTRA_ARGS! & goto :cli_done)
if /i "%ACTION%"=="upgrade-sh2-dc" (call :do_upgrade_sh2_dc !EXTRA_ARGS! & goto :cli_done)
if /i "%ACTION%"=="build-sh3" (call :do_build_sh3 !EXTRA_ARGS! & goto :cli_done)
if /i "%ACTION%"=="build-all" (call :do_build_all !EXTRA_ARGS! & goto :cli_done)
if /i "%ACTION%"=="play-launcher" (call :do_play_launcher & exit /b !ERRORLEVEL!)
if /i "%ACTION%"=="play-koudelka" (call :do_play_koudelka !EXTRA_ARGS! & exit /b !ERRORLEVEL!)
if /i "%ACTION%"=="play-sh1" (call :do_play_sh1 !EXTRA_ARGS! & exit /b !ERRORLEVEL!)
if /i "%ACTION%"=="play-sh2" (call :do_play_sh2 !EXTRA_ARGS! & exit /b !ERRORLEVEL!)
if /i "%ACTION%"=="play-sh3" (call :do_play_sh3 !EXTRA_ARGS! & exit /b !ERRORLEVEL!)
if /i "%ACTION%"=="bake-vu1" (call :menu_bake_vu1 & goto :cli_done)
if /i "%ACTION%"=="toolchain" (call :ensure_toolchain_force & goto :cli_done)
echo Unknown command: "%ACTION%"
echo Usage: "%~nx0" [wizard^|build-koudelka^|build-sh1^|build-sh2^|build-sh3^|build-all^|play-launcher^|play-koudelka^|play-sh1^|play-sh2^|play-sh3^|bake-vu1^|toolchain]
pause
exit /b 2

:cli_done
set "CLI_ERR=%ERRORLEVEL%"
if "%CLI_ERR%"=="10" exit /b 0
echo.
if not "%CLI_ERR%"=="0" echo Command "%ACTION%" finished with exit code %CLI_ERR%.
pause
exit /b %CLI_ERR%

:: ========================================================================
:: Active Install Dir & Status Detection
:: ========================================================================
:refresh_install_dir
set "INSTALL_DIR=%ROOT:~0,-1%"
if exist "%ROOT%tools\portable\python\python.exe" (
    for /f "usebackq delims=" %%D in (`"%ROOT%tools\portable\python\python.exe" "%ROOT%scripts\nocturne_setup.py" --print-install-dir 2^>nul`) do (
        if not "%%D"=="" set "INSTALL_DIR=%%D"
    )
)
exit /b 0

:detect_status
set "ST_LNCH=[NOT BUILT]"
set "ST_KDL=[NOT STAGED]"
set "ST_SH1=[NOT BUILT]"
set "ST_SH2=[NOT BUILT]"
set "ST_SH3=[NOT BUILT]"
if exist "%INSTALL_DIR%\Shadow Hearts Nocturne.exe" set "ST_LNCH=[READY]    "
if exist "%INSTALL_DIR%\games\koudelka\disc_files\disc_manifest.tsv" set "ST_KDL=[4 DISCS]  "
if exist "%INSTALL_DIR%\games\koudelka\Koudelka.exe" set "ST_KDL=[READY]    "
if exist "%INSTALL_DIR%\games\koudelka\game.dll" if exist "%INSTALL_DIR%\engine\nocturne_psx.exe" set "ST_KDL=[READY]    "
if exist "%INSTALL_DIR%\games\sh1\SH1.exe" set "ST_SH1=[READY]    "
if exist "%INSTALL_DIR%\games\sh1\game.dll" if exist "%INSTALL_DIR%\engine\nocturne_ps2.exe" set "ST_SH1=[READY]    "
if exist "%INSTALL_DIR%\games\sh2-covenant\SH2C.exe" set "ST_SH2=[READY]    "
if exist "%INSTALL_DIR%\games\sh2-covenant\game.dll" if exist "%INSTALL_DIR%\engine\nocturne_ps2.exe" set "ST_SH2=[READY]    "
if exist "%INSTALL_DIR%\games\sh3-ftnw\SH3.exe" set "ST_SH3=[READY]    "
if exist "%INSTALL_DIR%\games\sh3-ftnw\game.dll" if exist "%INSTALL_DIR%\engine\nocturne_ps2.exe" set "ST_SH3=[READY]    "
exit /b 0

:: ========================================================================
:: Portable Toolchain & Repo Files Bootstrap
:: ========================================================================
:ensure_repo_files
if exist "%ROOT%scripts\portable.ps1" if exist "%ROOT%scripts\nocturne_setup.py" exit /b 0
echo.
echo ========================================================================
echo   SHADOW HEARTS: NOCTURNE - Missing or Outdated Setup Files Detected
echo ========================================================================
echo   Folder: %ROOT%
echo.
echo   This folder does not have the latest Nocturne scripts/source files yet
echo   (for example, if only this .cmd file was copied here or git pull was
echo   not run yet).
echo.
echo   We can download the latest Shadow Hearts: Nocturne files from GitHub
echo   into this folder now (all existing games, saves, keys.ini, and tools
echo   are safely preserved).
echo.
set "DL_CHOICE="
set /p "DL_CHOICE=Download latest files from GitHub now? [Y/n]: "
if /i "%DL_CHOICE%"=="n" exit /b 1
if /i "%DL_CHOICE%"=="no" exit /b 1
echo.
"%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe" -NoLogo -NoProfile -ExecutionPolicy Bypass -Command "$ErrorActionPreference='Stop'; [Net.ServicePointManager]::SecurityProtocol=[Net.SecurityProtocolType]::Tls12; $dest=$env:ROOT.TrimEnd('\'); $tmp=Join-Path $env:TEMP ('nocturne_self_' + [Guid]::NewGuid().ToString('N').Substring(0,8)); $zip=Join-Path $tmp 'repo.zip'; $ext=Join-Path $tmp 'ext'; New-Item -ItemType Directory -Force -Path $ext | Out-Null; try { Write-Host '  Downloading NfgOdin/Shadow-Hearts-Nocturne (nightly)...' -ForegroundColor Cyan; Invoke-WebRequest -Uri 'https://github.com/NfgOdin/Shadow-Hearts-Nocturne/archive/refs/heads/nightly.zip' -OutFile $zip -UseBasicParsing; Write-Host '  Unpacking repository files...' -ForegroundColor Cyan; $tar=Join-Path $env:SystemRoot 'System32\tar.exe'; if (Test-Path $tar) { & $tar -xf $zip -C $ext; if ($LASTEXITCODE -ne 0) { throw 'tar failed' } } else { Add-Type -AssemblyName System.IO.Compression.FileSystem; [IO.Compression.ZipFile]::ExtractToDirectory($zip, $ext) }; $top=@(Get-ChildItem -LiteralPath $ext -Directory)[0].FullName; Write-Host '  Syncing source files (saves & games preserved)...' -ForegroundColor Cyan; $null = & robocopy.exe $top $dest /E /R:2 /W:1 /NFL /NDL /NJH /NJS /NP; if ($LASTEXITCODE -ge 8) { throw 'robocopy failed' }; Write-Host '  Download complete!' -ForegroundColor Green } finally { Remove-Item -LiteralPath $tmp -Recurse -Force -ErrorAction SilentlyContinue }"
if errorlevel 1 (
    echo.
    echo Failed to download repository files. Check your internet connection and try again.
    exit /b 1
)
if not exist "%ROOT%scripts\nocturne_setup.py" (
    echo.
    echo Expected setup scripts still missing after download.
    exit /b 1
)
exit /b 0

:ensure_toolchain
call :ensure_repo_files || exit /b 1
set "NEED_TOOLS=0"
if not exist "%ROOT%tools\portable\python\python.exe" set "NEED_TOOLS=1"
if not exist "%ROOT%tools\portable\llvm-mingw\bin\clang++.exe" set "NEED_TOOLS=1"
if "%NEED_TOOLS%"=="0" (
    "%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe" -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%ROOT%scripts\portable.ps1" -Check >nul 2>&1
    if errorlevel 1 set "NEED_TOOLS=1"
)
if "%NEED_TOOLS%"=="1" (
    echo.
    echo ========================================================================
    echo   First-time setup: downloading portable build tools into tools\portable
    echo   Clang, CMake, Ninja, Python, MinGit, BusyBox - verified by SHA-256
    echo ========================================================================
    echo.
    "%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe" -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%ROOT%scripts\portable.ps1"
    if errorlevel 1 (
        echo.
        echo Failed to download portable toolchain.
        exit /b 1
    )
)
if not exist "%ROOT%tools\portable\python\python.exe" (
    echo.
    echo ERROR: Portable Python was not found at "%ROOT%tools\portable\python\python.exe".
    exit /b 1
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
:do_wizard
call :ensure_toolchain || exit /b 1
"%ROOT%tools\portable\python\python.exe" -u "%ROOT%scripts\nocturne_setup.py" --wizard %*
exit /b %ERRORLEVEL%

:do_update
call :ensure_toolchain || exit /b 1
echo.
"%ROOT%tools\portable\python\python.exe" -u "%ROOT%scripts\nocturne_update.py" %*
set "BERR=%ERRORLEVEL%"
if not "%BERR%"=="0" if not "%BERR%"=="10" echo Update failed with exit code %BERR%.
exit /b %BERR%

:do_check_update
call :ensure_toolchain || exit /b 1
"%ROOT%tools\portable\python\python.exe" -u "%ROOT%scripts\nocturne_update.py" --check %*
exit /b %ERRORLEVEL%

:do_build_koudelka
call :ensure_toolchain || exit /b 1
echo.
"%ROOT%tools\portable\python\python.exe" -u "%ROOT%scripts\koudelka_build.py" %*
set "BERR=%ERRORLEVEL%"
if not "%BERR%"=="0" if not "%BERR%"=="10" echo Koudelka setup failed with exit code %BERR%.
exit /b %BERR%

:do_build_sh1
call :ensure_toolchain || exit /b 1
echo.
"%ROOT%tools\portable\python\python.exe" -u "%ROOT%scripts\sh1_build.py" %*
set "BERR=%ERRORLEVEL%"
if not "%BERR%"=="0" if not "%BERR%"=="10" echo Build failed with exit code %BERR%.
exit /b %BERR%

:do_build_sh2
call :ensure_toolchain || exit /b 1
echo.
"%ROOT%tools\portable\python\python.exe" -u "%ROOT%scripts\sh2_build.py" %*
set "BERR=%ERRORLEVEL%"
if not "%BERR%"=="0" if not "%BERR%"=="10" echo Build failed with exit code %BERR%.
exit /b %BERR%

:do_build_sh3
call :ensure_toolchain || exit /b 1
echo.
"%ROOT%tools\portable\python\python.exe" -u "%ROOT%scripts\sh3_build.py" %*
set "BERR=%ERRORLEVEL%"
if not "%BERR%"=="0" if not "%BERR%"=="10" echo Build failed with exit code %BERR%.
exit /b %BERR%

:do_upgrade_sh2_dc
call :ensure_toolchain || exit /b 1
echo.
"%ROOT%tools\portable\python\python.exe" -u "%ROOT%scripts\sh2_dc_upgrade.py" %*
set "BERR=%ERRORLEVEL%"
if not "%BERR%"=="0" if not "%BERR%"=="10" echo Director's Cut upgrade failed with exit code %BERR%.
exit /b %BERR%

:do_build_all
call :ensure_toolchain || exit /b 1
echo.
"%ROOT%tools\portable\python\python.exe" -u "%ROOT%scripts\nocturne_setup.py" --build-all %*
set "BERR=%ERRORLEVEL%"
if not "%BERR%"=="0" if not "%BERR%"=="10" echo Build All failed with exit code %BERR%.
exit /b %BERR%

:: ========================================================================
:: Direct Launch Targets
:: ========================================================================
:do_play_launcher
call :refresh_install_dir
if not exist "%INSTALL_DIR%\Shadow Hearts Nocturne.exe" (
    echo.
    echo Shadow Hearts Nocturne.exe has not been built yet.
    echo Please run [W] Setup Walkthrough or build at least one game first.
    pause
    exit /b 1
)
start "" "%INSTALL_DIR%\Shadow Hearts Nocturne.exe"
exit /b 0

:do_play_koudelka
call :refresh_install_dir
set "PATH=%INSTALL_DIR%;%ROOT%tools\portable\llvm-mingw\bin;%PATH%"
set "GAME_DIR=%INSTALL_DIR%\games\koudelka"
if exist "%GAME_DIR%\game.dll" if exist "%INSTALL_DIR%\engine\nocturne_psx.exe" (
    start "" "%INSTALL_DIR%\engine\nocturne_psx.exe" --game-dll "%GAME_DIR%\game.dll" --game-dir "%GAME_DIR%" %*
    exit /b 0
)
set "EXE=%GAME_DIR%\Koudelka.exe"
if not exist "%EXE%" set "EXE=%ROOT%build\native-koudelka\psxrun.exe"
if not exist "%EXE%" (
    echo.
    echo Koudelka (1898) has not been built yet. Choose option [K] or [W] first.
    pause
    exit /b 1
)
start "" "%EXE%" "%GAME_DIR%" %*
exit /b 0

:do_play_sh1
call :refresh_install_dir
set "PATH=%INSTALL_DIR%;%ROOT%tools\portable\llvm-mingw\bin;%PATH%"
set "PS2_WINDOW_TITLE=Shadow Hearts"
set "PS2_SCREENSHOTS_DIR=%USERPROFILE%\Pictures\Shadow Hearts Nocturne\Shadow Hearts"
set "PS2_SCREENSHOT_PREFIX=ShadowHearts"
set "GAME_DIR=%INSTALL_DIR%\games\sh1"
set "DISC1=%GAME_DIR%\disc_files"
if not exist "%DISC1%\disc_manifest.tsv" set "DISC1=%GAME_DIR%\disc.iso"
if exist "%GAME_DIR%\game.dll" if exist "%INSTALL_DIR%\engine\nocturne_ps2.exe" (
    start "" "%INSTALL_DIR%\engine\nocturne_ps2.exe" --game sh1 --game-dll "%GAME_DIR%\game.dll" --disc "%DISC1%" --game-dir "%GAME_DIR%" --memcard "%GAME_DIR%\memcard.ps2" --window %*
    exit /b 0
)
set "EXE=%GAME_DIR%\SH1.exe"
if not exist "%EXE%" set "EXE=%ROOT%build\native-sh1\ps2run.exe"
if not exist "%EXE%" (
    echo.
    echo Shadow Hearts (1913) has not been built yet. Choose option [1] or [W] first.
    pause
    exit /b 1
)
start "" "%EXE%" --disc "%DISC1%" --game-dir "%GAME_DIR%" --memcard "%GAME_DIR%\memcard.ps2" --window %*
exit /b 0

:do_play_sh2
call :refresh_install_dir
set "PATH=%INSTALL_DIR%;%ROOT%tools\portable\llvm-mingw\bin;%PATH%"
set "PS2_WINDOW_TITLE=Shadow Hearts: Covenant"
set "PS2_SCREENSHOTS_DIR=%USERPROFILE%\Pictures\Shadow Hearts Nocturne\Covenant"
set "PS2_SCREENSHOT_PREFIX=ShadowHeartsCovenant"
set "GAME_DIR=%INSTALL_DIR%\games\sh2-covenant"
set "DISC1=%GAME_DIR%\disc1_files"
if not exist "%DISC1%\disc_manifest.tsv" set "DISC1=%GAME_DIR%\disc.iso"
set "DISC2="
if exist "%GAME_DIR%\disc2_files\disc_manifest.tsv" set "DISC2=%GAME_DIR%\disc2_files"
if not defined DISC2 if exist "%GAME_DIR%\disc2.iso" set "DISC2=%GAME_DIR%\disc2.iso"
if exist "%GAME_DIR%\game.dll" if exist "%INSTALL_DIR%\engine\nocturne_ps2.exe" (
    if defined DISC2 (
        start "" "%INSTALL_DIR%\engine\nocturne_ps2.exe" --game sh2 --game-dll "%GAME_DIR%\game.dll" --disc "%DISC1%" --disc2 "%DISC2%" --game-dir "%GAME_DIR%" --memcard "%GAME_DIR%\memcard.ps2" --window %*
    ) else (
        start "" "%INSTALL_DIR%\engine\nocturne_ps2.exe" --game sh2 --game-dll "%GAME_DIR%\game.dll" --disc "%DISC1%" --game-dir "%GAME_DIR%" --memcard "%GAME_DIR%\memcard.ps2" --window %*
    )
    exit /b 0
)
set "EXE=%GAME_DIR%\SH2C.exe"
if not exist "%EXE%" set "EXE=%ROOT%build\native-sh2-covenant\ps2run.exe"
if not exist "%EXE%" (
    echo.
    echo Shadow Hearts: Covenant has not been built yet. Choose option [2] or [W] first.
    pause
    exit /b 1
)
if defined DISC2 (
    start "" "%EXE%" --disc "%DISC1%" --disc2 "%DISC2%" --game-dir "%GAME_DIR%" --memcard "%GAME_DIR%\memcard.ps2" --window %*
) else (
    start "" "%EXE%" --disc "%DISC1%" --game-dir "%GAME_DIR%" --memcard "%GAME_DIR%\memcard.ps2" --window %*
)
exit /b 0

:do_play_sh3
call :refresh_install_dir
set "PATH=%INSTALL_DIR%;%ROOT%tools\portable\llvm-mingw\bin;%PATH%"
set "PS2_WINDOW_TITLE=Shadow Hearts: From the New World"
set "PS2_SCREENSHOTS_DIR=%USERPROFILE%\Pictures\Shadow Hearts Nocturne\From the New World"
set "PS2_SCREENSHOT_PREFIX=ShadowHeartsNewWorld"
set "GAME_DIR=%INSTALL_DIR%\games\sh3-ftnw"
set "DISC1=%GAME_DIR%\disc_files"
if not exist "%DISC1%\disc_manifest.tsv" set "DISC1=%GAME_DIR%\disc.iso"
if exist "%GAME_DIR%\game.dll" if exist "%INSTALL_DIR%\engine\nocturne_ps2.exe" (
    start "" "%INSTALL_DIR%\engine\nocturne_ps2.exe" --game sh3 --game-dll "%GAME_DIR%\game.dll" --disc "%DISC1%" --game-dir "%GAME_DIR%" --memcard "%GAME_DIR%\memcard.ps2" --window %*
    exit /b 0
)
set "EXE=%GAME_DIR%\SH3.exe"
if not exist "%EXE%" set "EXE=%ROOT%build\native-sh3-ftnw\ps2run.exe"
if not exist "%EXE%" (
    echo.
    echo Shadow Hearts: From the New World has not been built yet. Choose option [3] or [W] first.
    pause
    exit /b 1
)
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
call :refresh_install_dir
echo.
echo   --- Offline Disc Texture ^& FMV Extraction / AI Upscaling ---
echo     [1] Shadow Hearts (1913)    - Dump All Textures (PNG)
echo     [2] Shadow Hearts (1913)    - Dump + 4x AI Upscale Textures
echo     [3] Shadow Hearts (1913)    - Dump All 19 FMVs (MP4)
echo     [4] Shadow Hearts: Covenant - Dump All Textures (PNG)
echo     [5] Shadow Hearts: Covenant - Dump + 4x AI Upscale Textures
echo     [6] Shadow Hearts: Covenant - Dump All FMVs (MP4)
echo     [7] From the New World      - Dump All Textures (PNG)
echo     [8] From the New World      - Dump + 4x AI Upscale Textures
echo     [9] From the New World      - Dump All FMVs (MP4)
echo     [0] Back
echo.
set "PK_CH="
set /p "PK_CH=Select operation [0-9]: "
if "%PK_CH%"=="1" ("%ROOT%tools\portable\python\python.exe" -u "%ROOT%scripts\sh1_extract_pkb.py" --disc "%INSTALL_DIR%\games\sh1\disc_files\layer0" --extract-tex "%INSTALL_DIR%\games\sh1\dumps\textures" --clean & pause)
if "%PK_CH%"=="2" ("%ROOT%tools\portable\python\python.exe" -u "%ROOT%scripts\sh1_extract_pkb.py" --disc "%INSTALL_DIR%\games\sh1\disc_files\layer0" --extract-tex "%INSTALL_DIR%\games\sh1\dumps\textures" --clean --upscale --output "%INSTALL_DIR%\games\sh1\mods\textures\SH1_HD_Pack" --scale 4 & pause)
if "%PK_CH%"=="3" ("%ROOT%tools\portable\python\python.exe" -u "%ROOT%scripts\sh1_extract_pkb.py" --disc "%INSTALL_DIR%\games\sh1\disc_files\layer0" --extract-fmv "%INSTALL_DIR%\games\sh1\dumps\fmv" --clean & pause)
if "%PK_CH%"=="4" ("%ROOT%tools\portable\python\python.exe" -u "%ROOT%scripts\sh2_extract_pkb.py" --disc "%INSTALL_DIR%\games\sh2-covenant\disc1_files\layer0" --extract-tex "%INSTALL_DIR%\games\sh2-covenant\dumps\textures" --clean & pause)
if "%PK_CH%"=="5" ("%ROOT%tools\portable\python\python.exe" -u "%ROOT%scripts\sh2_extract_pkb.py" --disc "%INSTALL_DIR%\games\sh2-covenant\disc1_files\layer0" --extract-tex "%INSTALL_DIR%\games\sh2-covenant\dumps\textures" --clean --upscale --output "%INSTALL_DIR%\games\sh2-covenant\mods\textures\SH2_HD_Pack" --scale 4 & pause)
if "%PK_CH%"=="6" ("%ROOT%tools\portable\python\python.exe" -u "%ROOT%scripts\sh2_extract_pkb.py" --disc "%INSTALL_DIR%\games\sh2-covenant\disc1_files\layer0" --extract-fmv "%INSTALL_DIR%\games\sh2-covenant\dumps\fmv" --clean & pause)
if "%PK_CH%"=="7" ("%ROOT%tools\portable\python\python.exe" -u "%ROOT%scripts\sh3_extract_pkb.py" --disc "%INSTALL_DIR%\games\sh3-ftnw\disc_files\layer0" --extract-tex "%INSTALL_DIR%\games\sh3-ftnw\dumps\textures" --clean & pause)
if "%PK_CH%"=="8" ("%ROOT%tools\portable\python\python.exe" -u "%ROOT%scripts\sh3_extract_pkb.py" --disc "%INSTALL_DIR%\games\sh3-ftnw\disc_files\layer0" --extract-tex "%INSTALL_DIR%\games\sh3-ftnw\dumps\textures" --clean --upscale --output "%INSTALL_DIR%\games\sh3-ftnw\mods\textures\SH3_HD_Pack" --scale 4 & pause)
if "%PK_CH%"=="9" ("%ROOT%tools\portable\python\python.exe" -u "%ROOT%scripts\sh3_extract_pkb.py" --disc "%INSTALL_DIR%\games\sh3-ftnw\disc_files\layer0" --extract-fmv "%INSTALL_DIR%\games\sh3-ftnw\dumps\fmv" --clean & pause)
exit /b 0
