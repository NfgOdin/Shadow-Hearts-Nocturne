@echo off
setlocal
title Shadow Hearts: Nocturne - Standalone Installer
set "NOCTURNE_SELF=%~f0"
"%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe" -NoLogo -NoProfile -ExecutionPolicy Bypass -STA -Command "$c = Get-Content -LiteralPath $env:NOCTURNE_SELF -Raw; $m = '#___NOCTURNE_PS___'; $i = $c.LastIndexOf($m); & ([ScriptBlock]::Create($c.Substring($i + $m.Length)))"
exit /b %ERRORLEVEL%
#___NOCTURNE_PS___
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

$RepoSlug = 'NfgOdin/Shadow-Hearts-Nocturne'
$DefaultDir = 'C:\ShadowHeartsNocturne'

function Write-Banner([string]$Title, [string]$Sub = '') {
    Write-Host ''
    Write-Host ('=' * 74) -ForegroundColor Yellow
    Write-Host "  $Title" -ForegroundColor Yellow
    if ($Sub) { Write-Host "  $Sub" -ForegroundColor Gray }
    Write-Host ('=' * 74) -ForegroundColor Yellow
}

function Pause-Exit([int]$Code = 0) {
    if ($env:NOCTURNE_NO_PAUSE -ne '1') {
        Write-Host ''
        Write-Host '  Press any key to close...' -ForegroundColor Gray -NoNewline
        try { $null = [Console]::ReadKey($true) } catch { $null = Read-Host }
        Write-Host ''
    }
    exit $Code
}

function Pick-Folder([string]$Title, [string]$Initial = '') {
    Add-Type -AssemblyName System.Windows.Forms
    $owner = New-Object System.Windows.Forms.Form
    $owner.TopMost = $true
    $owner.StartPosition = 'CenterScreen'
    $owner.Width = 1; $owner.Height = 1; $owner.ShowInTaskbar = $false
    $dlg = New-Object System.Windows.Forms.FolderBrowserDialog
    $dlg.Description = $Title
    $dlg.UseDescriptionForTitle = $true
    $dlg.ShowNewFolderButton = $true
    if ($Initial -and (Test-Path -LiteralPath $Initial)) { $dlg.SelectedPath = $Initial }
    $result = ''
    if ($dlg.ShowDialog($owner) -eq [System.Windows.Forms.DialogResult]::OK) {
        $result = $dlg.SelectedPath
    }
    $owner.Dispose()
    return $result
}

function Ensure-NocturneClosed {
    $watch = @('Shadow Hearts Nocturne', 'sh2launcher', 'nocturne_ps2', 'nocturne_psx', 'Koudelka', 'SH1', 'SH2C', 'SH3')
    $running = @(Get-Process -ErrorAction SilentlyContinue | Where-Object { $watch -contains $_.ProcessName })
    if ($running.Count -eq 0) { return $true }

    Write-Host ''
    Write-Host '  ----------------------------------------------------------------------' -ForegroundColor Yellow
    Write-Host '  APP CURRENTLY OPEN - MUST BE CLOSED TO CONTINUE' -ForegroundColor Yellow
    Write-Host '  ----------------------------------------------------------------------' -ForegroundColor Yellow
    Write-Host '  The following Shadow Hearts: Nocturne app(s) are currently running:'
    foreach ($p in ($running | Sort-Object ProcessName -Unique)) {
        Write-Host "    - $($p.ProcessName).exe" -ForegroundColor White
    }
    Write-Host ''
    while ($true) {
        $ans = (Read-Host '  Close the running app(s) automatically and continue? [Y/N]').Trim().ToLower()
        if ($ans -eq 'y' -or $ans -eq 'yes') {
            Write-Host '  Closing app(s) cleanly...' -ForegroundColor Cyan
            foreach ($p in $running) { try { $null = $p.CloseMainWindow() } catch {} }
            $deadline = (Get-Date).AddSeconds(3.5)
            while ((Get-Date) -lt $deadline) {
                $still = @(Get-Process -ErrorAction SilentlyContinue | Where-Object { $watch -contains $_.ProcessName })
                if ($still.Count -eq 0) { Start-Sleep -Milliseconds 300; Write-Host '  Closed cleanly.' -ForegroundColor Green; return $true }
                Start-Sleep -Milliseconds 250
            }
            $still = @(Get-Process -ErrorAction SilentlyContinue | Where-Object { $watch -contains $_.ProcessName })
            foreach ($p in $still) { try { Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue } catch {} }
            Start-Sleep -Milliseconds 500
            Write-Host '  Closed cleanly.' -ForegroundColor Green
            return $true
        }
        if ($ans -eq 'n' -or $ans -eq 'no') {
            Write-Host ''
            Write-Host '  Installation cancelled.' -ForegroundColor Yellow
            Write-Host '  Please re-run this installer once you are finished with the app.'
            Pause-Exit 0
        }
        Write-Host '  Please enter Y or N.'
    }
}

function Download-WithProgress([string]$Url, [string]$OutFile) {
    $tries = 3
    for ($try = 1; $try -le $tries; $try++) {
        try {
            $req = [Net.HttpWebRequest]::Create($Url)
            $req.UserAgent = 'ShadowHeartsNocturne-Bootstrap/1.0'
            $req.Proxy = [Net.WebRequest]::GetSystemWebProxy()
            $req.Proxy.Credentials = [Net.CredentialCache]::DefaultCredentials
            $req.Timeout = 60000
            $req.ReadWriteTimeout = 60000
            $resp = $req.GetResponse()
            try {
                $total = [int64]$resp.ContentLength
                $inStream = $resp.GetResponseStream()
                $outStream = New-Object IO.FileStream($OutFile, [IO.FileMode]::Create, [IO.FileAccess]::Write)
                try {
                    $buf = New-Object byte[] 131072
                    $done = [int64]0
                    $sw = [Diagnostics.Stopwatch]::StartNew()
                    $lastDraw = -200
                    $spin = @('|', '/', '-', '\')
                    $si = 0
                    while (($n = $inStream.Read($buf, 0, $buf.Length)) -gt 0) {
                        $outStream.Write($buf, 0, $n)
                        $done += $n
                        if ($sw.ElapsedMilliseconds -ge ($lastDraw + 120)) {
                            $lastDraw = $sw.ElapsedMilliseconds
                            $mb = '{0:0.0}' -f ($done / 1MB)
                            if ($total -gt 0) {
                                $pct = [Math]::Min(100.0, ($done * 100.0) / $total)
                                $fill = [int][Math]::Round(($pct / 100.0) * 24)
                                $bar = ('#' * $fill) + ('-' * (24 - $fill))
                                $tmb = '{0:0.0}' -f ($total / 1MB)
                                Write-Host ("`r  [Download] [{0}] {1,5:0.0}%  ({2} / {3} MB)  " -f $bar, $pct, $mb, $tmb) -NoNewline
                            } else {
                                $ch = $spin[$si % 4]; $si++
                                Write-Host ("`r  [Download] {0}  {1} MB downloaded...          " -f $ch, $mb) -NoNewline
                            }
                        }
                    }
                    $mbFinal = '{0:0.0}' -f ($done / 1MB)
                    Write-Host ("`r  [Download] [########################] 100.0% ({0} MB in {1:0.0}s)   " -f $mbFinal, $sw.Elapsed.TotalSeconds) -ForegroundColor Green
                } finally {
                    $outStream.Dispose()
                    $inStream.Dispose()
                }
            } finally {
                $resp.Close()
            }
            return
        } catch {
            Write-Host ''
            if ($try -lt $tries) {
                Write-Host "  Download interrupted ($($_.Exception.Message)). Retrying ($($try + 1)/$tries)..." -ForegroundColor Yellow
                Start-Sleep -Seconds 3
            } else {
                throw
            }
        }
    }
}

try {
    Write-Banner 'SHADOW HEARTS: NOCTURNE - STANDALONE INSTALLER' 'Downloads the latest port automatically. Zero manual tool installs required!'
    Write-Host '  What this single-file installer does:'
    Write-Host '    1. Downloads the latest Shadow Hearts: Nocturne source from GitHub'
    Write-Host '    2. Safely preserves any existing saves, keys.ini, mods & unpacked discs'
    Write-Host '    3. Launches the 6-step Setup Walkthrough (fetches Clang/CMake/Python/SDL2'
    Write-Host '       automatically - you only supply your own game disc images!)'
    Write-Host ''

    # 1. Choose channel (Nightly default until v1.0 promotion to main)
    $branch = $env:NOCTURNE_BRANCH
    if (-not $branch) {
        Write-Host '  Select release channel:' -ForegroundColor Cyan
        Write-Host '    [1] Nightly  (Latest updates, Koudelka PS1 + SH1-SH3, Setup Wizard)  [Recommended]' -ForegroundColor Green
        Write-Host '    [2] Main     (Stable branch)'
        $ch = (Read-Host "`n  Channel [1]").Trim()
        if ($ch -eq '2' -or $ch.ToLower() -eq 'main') { $branch = 'main' } else { $branch = 'nightly' }
    }

    # 2. Choose destination folder (short path avoids Windows 260-char Clang path limits)
    $dest = $env:NOCTURNE_DEST
    if (-not $dest) {
        Write-Host ''
        Write-Host '  Choose where to set up Shadow Hearts: Nocturne:' -ForegroundColor Cyan
        Write-Host "    Default: $DefaultDir  (short path recommended for fast builds)" -ForegroundColor Gray
        Write-Host '    Press [Enter] for default, [B] to browse, or paste a folder path.' -ForegroundColor Gray
        while ($true) {
            $raw = (Read-Host "`n  Folder [$DefaultDir]").Trim()
            if (-not $raw) { $dest = $DefaultDir; break }
            if ($raw.ToLower() -eq 'b') {
                $picked = Pick-Folder 'Choose Shadow Hearts: Nocturne Folder' $DefaultDir
                if ($picked) { $dest = $picked; break }
                Write-Host '    (Browse cancelled - press Enter for default, B to browse, or type a path.)' -ForegroundColor Yellow
                continue
            }
            $dest = $raw.Trim('"').Trim("'")
            break
        }
    }
    $dest = [IO.Path]::GetFullPath($dest)

    if (-not (Ensure-NocturneClosed)) { Pause-Exit 0 }

    New-Item -ItemType Directory -Force -Path $dest | Out-Null

    # 3. Download GitHub archive
    $zipUrl = "https://github.com/$RepoSlug/archive/refs/heads/$branch.zip"
    $tmpRoot = Join-Path $env:TEMP ("nocturne_bootstrap_" + [Guid]::NewGuid().ToString('N').Substring(0, 8))
    $zipFile = Join-Path $tmpRoot 'repo.zip'
    $extractDir = Join-Path $tmpRoot 'extract'
    New-Item -ItemType Directory -Force -Path $extractDir | Out-Null

    try {
        Write-Host ''
        Write-Host "  Fetching $RepoSlug ($branch) -> $dest" -ForegroundColor Cyan
        Download-WithProgress $zipUrl $zipFile

        Write-Host '  [Extract] Unpacking repository files...' -NoNewline
        $tarExe = Join-Path $env:SystemRoot 'System32\tar.exe'
        if (Test-Path -LiteralPath $tarExe) {
            & $tarExe -xf $zipFile -C $extractDir
            if ($LASTEXITCODE -ne 0) { throw "tar.exe extraction failed (exit $LASTEXITCODE)" }
        } else {
            Add-Type -AssemblyName System.IO.Compression.FileSystem
            [IO.Compression.ZipFile]::ExtractToDirectory($zipFile, $extractDir)
        }
        Write-Host ' OK' -ForegroundColor Green

        $topDirs = @(Get-ChildItem -LiteralPath $extractDir -Directory)
        if ($topDirs.Count -ne 1) { throw 'Unexpected archive structure from GitHub.' }
        $srcRoot = $topDirs[0].FullName

        # Sync source files into $dest while preserving all user data & caches
        # (games/, tools/portable/, third_party/, build/, generated/, install_config.json, launcher.ini, EXEs/DLLs)
        Write-Host '  [Sync]    Installing/updating source files (user saves & tools preserved)...' -NoNewline
        $robocopy = Join-Path $env:SystemRoot 'System32\robocopy.exe'
        if (Test-Path -LiteralPath $robocopy) {
            $null = & $robocopy $srcRoot $dest /E /R:2 /W:1 /NFL /NDL /NJH /NJS /NP
            if ($LASTEXITCODE -ge 8) { throw "robocopy failed (exit $LASTEXITCODE)" }
        } else {
            Copy-Item -Path (Join-Path $srcRoot '*') -Destination $dest -Recurse -Force
        }
        Write-Host ' OK' -ForegroundColor Green
    } finally {
        if (Test-Path -LiteralPath $tmpRoot) {
            Remove-Item -LiteralPath $tmpRoot -Recurse -Force -ErrorAction SilentlyContinue
        }
    }

    # Stamp update channel + latest GitHub commit SHA into install_config.json (best-effort)
    try {
        $cfgFile = Join-Path $dest 'install_config.json'
        $cfgObj = @{}
        if (Test-Path -LiteralPath $cfgFile) {
            $parsed = Get-Content -LiteralPath $cfgFile -Raw | ConvertFrom-Json
            foreach ($prop in $parsed.PSObject.Properties) { $cfgObj[$prop.Name] = $prop.Value }
        }
        $cfgObj['update_channel'] = $branch
        try {
            $apiReq = Invoke-RestMethod -Uri "https://api.github.com/repos/$RepoSlug/commits/$branch" -Headers @{ 'User-Agent' = 'ShadowHeartsNocturne-Bootstrap/1.0' } -TimeoutSec 6
            if ($apiReq.sha) {
                $cfgObj['installed_commit'] = [string]$apiReq.sha
                $rawDate = [string]$apiReq.commit.committer.date
                if ($rawDate.Length -ge 10) { $cfgObj['installed_date'] = $rawDate.Substring(0, 10) }
                $firstLine = ([string]$apiReq.commit.message -split "`n")[0].Trim()
                $cfgObj['installed_subject'] = $firstLine
            }
        } catch {}
        [System.IO.File]::WriteAllText($cfgFile, ($cfgObj | ConvertTo-Json -Depth 4), [System.Text.UTF8Encoding]::new($false))
    } catch {}

    $setupCmd = Join-Path $dest 'Shadow Hearts Nocturne Setup.cmd'
    if (-not (Test-Path -LiteralPath $setupCmd)) {
        throw "Expected setup script not found: $setupCmd"
    }

    Write-Host ''
    Write-Host "  Ready! Handing off to Shadow Hearts Nocturne Setup.cmd..." -ForegroundColor Green
    Write-Host ''

    if ($env:NOCTURNE_NO_LAUNCH -eq '1') {
        exit 0
    }

    Set-Location -LiteralPath $dest
    & $env:ComSpec /c "`"$setupCmd`""
    exit $LASTEXITCODE
} catch {
    Write-Host ''
    Write-Host "  [ERROR] $($_.Exception.Message)" -ForegroundColor Red
    Write-Host '  Check your internet connection and try running this installer again.' -ForegroundColor Yellow
    Pause-Exit 1
}
