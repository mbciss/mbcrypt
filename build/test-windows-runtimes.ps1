# test-windows-runtimes.ps1 - Download full PHP NTS+TS runtimes (8.0-8.5) and
# run end-to-end execution tests (mbcryptload decrypt + mbpak:// wrapper)
# against the freshly built DLLs in build/dist/.
#
# Author: CISSE Mohamed B
# This is a one-off verification script (not part of the normal build chain),
# written to close the gap noted in test/README.md: "Les variantes NTS ne
# sont pas verifiees a l'execution ici (aucun runtime PHP NTS installe
# localement)". Downloads full php.exe binaries (not just devel packs) for
# all 12 combinations PHP 8.0-8.5 x NTS/TS, mirroring build-linux-docker.sh's
# use of official php:8.X-cli / php:8.X-zts images on the Linux side.

$ProgressPreference = "SilentlyContinue"

$SCRIPT_DIR  = Split-Path -Parent $MyInvocation.MyCommand.Path
$PROJECT_DIR = Split-Path -Parent $SCRIPT_DIR
$DIST_DIR    = Join-Path $SCRIPT_DIR "dist"
$RT_DIR      = Join-Path $SCRIPT_DIR "php-runtimes"
$TEST_DIR    = Join-Path $PROJECT_DIR "test"

if (-not (Test-Path $RT_DIR)) { New-Item -ItemType Directory -Path $RT_DIR | Out-Null }

function Log-Step { param($m) Write-Host "`n==> $m" -ForegroundColor Cyan }
function Log-OK   { param($m) Write-Host "    [OK] $m" -ForegroundColor Green }
function Log-Warn { param($m) Write-Host "    [!!] $m" -ForegroundColor Yellow }
function Log-Fail { param($m) Write-Host "    [KO] $m" -ForegroundColor Red }

$MATRIX = @(
    @{ Ver="80"; FullVer="8.0.30"; Toolset="vs16" },
    @{ Ver="81"; FullVer="8.1.10"; Toolset="vs16" },
    @{ Ver="82"; FullVer="8.2.15"; Toolset="vs16" },
    @{ Ver="83"; FullVer="8.3.16"; Toolset="vs16" },
    @{ Ver="84"; FullVer="8.4.23"; Toolset="vs17" },
    @{ Ver="85"; FullVer="8.5.8";  Toolset="vs17" }
)
$THREAD_MODES = @("ts", "nts")

function Get-Runtime {
    param($FullVer, $Mode, $Toolset)

    $suffix = if ($Mode -eq "nts") { "-nts" } else { "" }
    $name   = "php-$FullVer$suffix-Win32-$Toolset-x64"
    $dest   = Join-Path $RT_DIR "$FullVer-$Mode"
    $exe    = Join-Path $dest "php.exe"

    if (Test-Path $exe) { return $dest }

    $zip = Join-Path $RT_DIR "$name.zip"
    $urls = @(
        "https://windows.php.net/downloads/releases/$name.zip",
        "https://windows.php.net/downloads/releases/archives/$name.zip"
    )
    $got = $false
    foreach ($url in $urls) {
        Write-Host "    Trying: $url"
        try {
            Invoke-WebRequest -Uri $url -OutFile $zip -UseBasicParsing -TimeoutSec 180
            $got = $true
            break
        } catch { Log-Warn "Failed: $url" }
    }
    if (-not $got) { return $null }

    if (-not (Test-Path $dest)) { New-Item -ItemType Directory -Path $dest | Out-Null }
    Expand-Archive -Path $zip -DestinationPath $dest -Force
    Remove-Item $zip -Force -ErrorAction SilentlyContinue
    return $dest
}

$results = @{}       # mbcrypt calcul.encrypted.php test
$mbpakResults = @{}  # mbpak_mount + mbpak:// wrapper test

foreach ($entry in $MATRIX) {
    foreach ($mode in $THREAD_MODES) {
        $ver = $entry.Ver
        $key = "$($entry.FullVer)-$mode"

        Log-Step "PHP $($entry.FullVer) $($mode.ToUpper())"

        $rt = Get-Runtime -FullVer $entry.FullVer -Mode $mode -Toolset $entry.Toolset
        if ($rt -eq $null) {
            Log-Fail "Cannot download runtime"
            $results[$key] = "SKIP"; $mbpakResults[$key] = "SKIP"
            continue
        }
        Log-OK "Runtime: $rt"

        $phpExe = Join-Path $rt "php.exe"
        $extDir = Join-Path $rt "ext"
        if (-not (Test-Path $extDir)) { New-Item -ItemType Directory -Path $extDir | Out-Null }

        $dll = Join-Path $DIST_DIR "php_mbcryptload_php$ver`_$mode.dll"
        if (-not (Test-Path $dll)) {
            Log-Fail "DLL missing: $dll"
            $results[$key] = "SKIP"; $mbpakResults[$key] = "SKIP"
            continue
        }
        Copy-Item $dll (Join-Path $extDir "php_mbcryptload.dll") -Force

        # Minimal php.ini: just enough to load the extension, no other modules
        # (avoids "zip already loaded"-type warnings from a stray system php.ini).
        $ini = Join-Path $rt "php-test.ini"
        @"
extension_dir = "ext"
extension = php_mbcryptload
"@ | Set-Content -Path $ini -Encoding ASCII

        # --- Test 1: mbcryptload decrypt + execute calcul.encrypted.php ---
        $testDir = Join-Path $TEST_DIR "php$ver"
        Push-Location $testDir
        $out1 = & $phpExe -n -c $ini index.php 2>&1
        $code1 = $LASTEXITCODE
        Pop-Location
        if ($code1 -eq 0 -and ($out1 -join "`n") -match "OK - calcul termine") {
            Log-OK "mbcrypt decrypt+execute: PASS"
            $results[$key] = "PASS"
        } else {
            Log-Fail "mbcrypt decrypt+execute: FAIL (exit $code1)"
            $out1 | ForEach-Object { Write-Host "        $_" -ForegroundColor DarkGray }
            $results[$key] = "FAIL"
        }

        # --- Test 2: mbpak_mount() + require "mbpak://..." ---
        $tmp = Join-Path $RT_DIR "mbpak-wrapper-test-$key"
        Remove-Item $tmp -Recurse -Force -ErrorAction SilentlyContinue
        New-Item -ItemType Directory -Path (Join-Path $tmp "src\lib") -Force | Out-Null
        Set-Content -Path (Join-Path $tmp "src\lib\greet.php") -Encoding ASCII -Value @"
<?php
function greet(`$name) { return "Bonjour, `$name (depuis mbpak://lib/greet.php)"; }
"@
        Set-Content -Path (Join-Path $tmp "src\main.php") -Encoding ASCII -Value @"
<?php
require "mbpak://lib/greet.php";
echo greet("PHP-$key") . "\n";
echo "mbpak_list() : " . count(mbpak_list()) . " fichier(s)\n";
echo "OK - test mbpak wrapper termine.\n";
"@
        $archive = Join-Path $tmp "app.mbpak"
        $mbpakExe = Join-Path $DIST_DIR "mbpak.exe"
        & $mbpakExe pack -p "WinRuntimeTest2026" -o $archive (Join-Path $tmp "src") | Out-Null

        Set-Content -Path (Join-Path $tmp "runner.php") -Encoding ASCII -Value @"
<?php
if (!mbpak_mount("$($archive -replace '\\','\\\\')", "WinRuntimeTest2026")) { fwrite(STDERR, "mount failed\n"); exit(1); }
require "mbpak://main.php";
"@

        $out2 = & $phpExe -n -c $ini (Join-Path $tmp "runner.php") 2>&1
        $code2 = $LASTEXITCODE
        if ($code2 -eq 0 -and ($out2 -join "`n") -match "OK - test mbpak wrapper termine") {
            Log-OK "mbpak:// wrapper: PASS"
            $mbpakResults[$key] = "PASS"
        } else {
            Log-Fail "mbpak:// wrapper: FAIL (exit $code2)"
            $out2 | ForEach-Object { Write-Host "        $_" -ForegroundColor DarkGray }
            $mbpakResults[$key] = "FAIL"
        }
        Remove-Item $tmp -Recurse -Force -ErrorAction SilentlyContinue
    }
}

Log-Step "Summary -- mbcryptload decrypt+execute (calcul.encrypted.php)"
foreach ($k in ($results.Keys | Sort-Object)) {
    $v = $results[$k]
    $c = if ($v -eq "PASS") { "Green" } else { "Red" }
    Write-Host ("    PHP {0} : {1}" -f $k, $v) -ForegroundColor $c
}

Log-Step "Summary -- mbpak:// stream wrapper (mount + require)"
foreach ($k in ($mbpakResults.Keys | Sort-Object)) {
    $v = $mbpakResults[$k]
    $c = if ($v -eq "PASS") { "Green" } else { "Red" }
    Write-Host ("    PHP {0} : {1}" -f $k, $v) -ForegroundColor $c
}
