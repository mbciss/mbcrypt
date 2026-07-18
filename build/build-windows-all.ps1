# build-windows-all.ps1 - Compile mbcryptload.dll for PHP 8.0 -> 8.5, TS and NTS
# Author: CISSE Mohamed B
# Usage: powershell.exe -ExecutionPolicy Bypass -File build-windows-all.ps1
#
# Unlike build-windows.ps1 (which links against a full Laragon PHP install),
# this script is self-contained: it uses the .lib shipped inside each PHP
# devel pack, so no full PHP installation is required for a given version.

$ProgressPreference = "SilentlyContinue"

# ---------------------------------------------------------------------------
# Paths
# ---------------------------------------------------------------------------
$SCRIPT_DIR  = Split-Path -Parent $MyInvocation.MyCommand.Path
$PROJECT_DIR = Split-Path -Parent $SCRIPT_DIR
$DIST_DIR    = Join-Path $SCRIPT_DIR "dist"
$DEVEL_DIR   = Join-Path $SCRIPT_DIR "devel-packs"

$SRC1 = Join-Path $PROJECT_DIR "mbcrypt\mbcryptload.c"
$SRC2 = Join-Path $PROJECT_DIR "mbcrypt\mbcrypt_lib.c"
$SRC3 = Join-Path $PROJECT_DIR "mbcrypt\mbpak_lib.c"   # .mbpak archive support, always linked into mbcryptload.c

# ---------------------------------------------------------------------------
# Toolchains
# ---------------------------------------------------------------------------
# VS 2019 BuildTools (vs16) -- PHP 8.0 to 8.3
$VS16_MSVC   = "C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\VC\Tools\MSVC\14.29.30133"
$VS16_CL     = "$VS16_MSVC\bin\Hostx64\x64\cl.exe"
$VS16_INCL   = "$VS16_MSVC\include"
$VS16_LIB    = "$VS16_MSVC\lib\x64"
$VS16_ZLIB_I = "C:\laragon\bin\apache\httpd-2.4.54-win64-VS16\include"
$VS16_ZLIB_L = "C:\laragon\bin\apache\httpd-2.4.54-win64-VS16\lib\zlib.lib"

# VS 2022 BuildTools/Community (vs17) -- PHP 8.4 and 8.5
$VS17_MSVC   = "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.43.34808"
$VS17_CL     = "$VS17_MSVC\bin\Hostx64\x64\cl.exe"
$VS17_INCL   = "$VS17_MSVC\include"
$VS17_LIB    = "$VS17_MSVC\lib\x64"
$VS17_ZLIB_I = "C:\laragon\bin\apache\httpd-2.4.62-240904-win64-VS17\include"
$VS17_ZLIB_L = "C:\laragon\bin\apache\httpd-2.4.62-240904-win64-VS17\lib\zlib.lib"

# Windows SDK (shared across toolchains)
$WINSDK_VER  = "10.0.22621.0"
$WINSDK_ROOT = "C:\Program Files (x86)\Windows Kits\10"
$WINSDK_INC  = "$WINSDK_ROOT\Include\$WINSDK_VER"
$WINSDK_LIB  = "$WINSDK_ROOT\Lib\$WINSDK_VER"

# OpenSSL Win64 (shared, MD runtime, binary-compatible across VS2019/2022 UCRT)
$SSL_INCL = "C:\Program Files\OpenSSL-Win64\include"
$SSL_LDIR = "C:\Program Files\OpenSSL-Win64\lib\VC\x64\MD"
$HAS_SSL  = Test-Path "$SSL_INCL\openssl\evp.h"

# ---------------------------------------------------------------------------
# Matrix: PHP version x thread-safety
# ---------------------------------------------------------------------------
$MATRIX = @(
    @{ Ver="80"; FullVer="8.0.30"; Toolset="vs16" },
    @{ Ver="81"; FullVer="8.1.10"; Toolset="vs16" },
    @{ Ver="82"; FullVer="8.2.15"; Toolset="vs16" },
    @{ Ver="83"; FullVer="8.3.16"; Toolset="vs16" },
    @{ Ver="84"; FullVer="8.4.23"; Toolset="vs17" },
    @{ Ver="85"; FullVer="8.5.8";  Toolset="vs17" }
)
$THREAD_MODES = @("ts", "nts")

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------
function Log-Step { param($m) Write-Host "`n==> $m" -ForegroundColor Cyan }
function Log-OK   { param($m) Write-Host "    [OK] $m" -ForegroundColor Green }
function Log-Warn { param($m) Write-Host "    [!!] $m" -ForegroundColor Yellow }
function Log-Fail { param($m) Write-Host "    [KO] $m" -ForegroundColor Red }

if (-not (Test-Path $DEVEL_DIR)) { New-Item -ItemType Directory -Path $DEVEL_DIR | Out-Null }
if (-not (Test-Path $DIST_DIR))  { New-Item -ItemType Directory -Path $DIST_DIR  | Out-Null }

Log-Step "Environment check"
if (-not (Test-Path $VS16_CL)) { Log-Fail "cl.exe (vs16) not found: $VS16_CL"; exit 1 }
Log-OK "cl.exe VS2019 (vs16): $VS16_CL"
if (-not (Test-Path $VS17_CL)) { Log-Fail "cl.exe (vs17) not found: $VS17_CL"; exit 1 }
Log-OK "cl.exe VS2022 (vs17): $VS17_CL"
if ($HAS_SSL) { Log-OK "OpenSSL: $SSL_INCL" } else { Log-Warn "OpenSSL not found -- AES-256-GCM disabled (XOR only)" }

# ---------------------------------------------------------------------------
# Download one devel pack (TS or NTS)
# ---------------------------------------------------------------------------
function Get-DevelPack {
    param($FullVer, $Mode)

    $suffix = if ($Mode -eq "nts") { "-nts" } else { "" }
    $name   = "php-devel-pack-$FullVer$suffix-Win32-vs16-x64"   # placeholder, real name built below
    $dest   = Join-Path $DEVEL_DIR "$FullVer-$Mode"
    $incl   = Join-Path $dest "include"

    if (Test-Path $incl) {
        Log-OK "Devel pack $FullVer ($Mode) already extracted."
        return $dest
    }

    # Toolset suffix in the filename depends on the PHP branch (vs16 vs vs17)
    $entry = $MATRIX | Where-Object { $_.FullVer -eq $FullVer } | Select-Object -First 1
    $toolset = $entry.Toolset
    $name = "php-devel-pack-$FullVer$suffix-Win32-$toolset-x64"
    $zip  = Join-Path $DEVEL_DIR "$name.zip"

    $urls = @(
        "https://windows.php.net/downloads/releases/$name.zip",
        "https://windows.php.net/downloads/releases/archives/$name.zip"
    )

    $got = $false
    foreach ($url in $urls) {
        Write-Host "    Trying: $url"
        try {
            Invoke-WebRequest -Uri $url -OutFile $zip -UseBasicParsing -TimeoutSec 120
            $got = $true
            Log-OK "Downloaded: $zip"
            break
        } catch {
            Log-Warn "Failed: $url"
        }
    }

    if (-not $got) {
        Log-Fail "Cannot download devel pack $FullVer ($Mode)"
        return $null
    }

    if (-not (Test-Path $dest)) { New-Item -ItemType Directory -Path $dest | Out-Null }
    Expand-Archive -Path $zip -DestinationPath $dest -Force

    $inner = Get-ChildItem -Path $dest -Directory | Select-Object -First 1
    if ($inner) {
        Get-ChildItem -Path $inner.FullName | ForEach-Object {
            Move-Item -Path $_.FullName -Destination $dest -Force
        }
        Remove-Item $inner.FullName -Recurse -Force -ErrorAction SilentlyContinue
    }

    return $dest
}

# ---------------------------------------------------------------------------
# Compile one PHP version / thread-safety combination
# ---------------------------------------------------------------------------
function Build-Ext {
    param($Entry, $Mode, $DevelDir)

    $ver     = $Entry.Ver
    $toolset = $Entry.Toolset
    $isTs    = ($Mode -eq "ts")

    if ($toolset -eq "vs16") {
        $CL = $VS16_CL; $MSVC_INCL = $VS16_INCL; $MSVC_LIB = $VS16_LIB
        $ZLIB_INCL = $VS16_ZLIB_I; $ZLIB_LIB = $VS16_ZLIB_L
    } else {
        $CL = $VS17_CL; $MSVC_INCL = $VS17_INCL; $MSVC_LIB = $VS17_LIB
        $ZLIB_INCL = $VS17_ZLIB_I; $ZLIB_LIB = $VS17_ZLIB_L
    }

    $phpLibName = if ($isTs) { "php8ts.lib" } else { "php8.lib" }
    $phpLib     = Join-Path $DevelDir "lib\$phpLibName"
    $phpInc     = Join-Path $DevelDir "include"
    $outDll     = Join-Path $DIST_DIR "php_mbcryptload_php$ver`_$Mode.dll"

    Log-Step "Compiling PHP $($Entry.FullVer) $($Mode.ToUpper()) ($toolset)"

    if (-not (Test-Path $phpLib))  { Log-Fail "$phpLibName missing: $phpLib"; return $false }
    if (-not (Test-Path $phpInc))  { Log-Fail "PHP headers missing in: $DevelDir"; return $false }

    $args = @()
    $args += "/LD"; $args += "/MD"; $args += "/Ox"; $args += "/W3"; $args += "/nologo"

    $args += "/DWIN32"; $args += "/D_WINDOWS"; $args += "/DWIN64"
    $args += "/DPHP_WIN32"; $args += "/DZEND_WIN32"
    if ($isTs) { $args += "/DZTS"; $args += "/DUSING_ZTS=1" }
    $args += "/DZEND_DEBUG=0"
    $args += "/DCOMPILE_DL_MBCRYPTLOAD"
    $args += "/DZLIB_WINAPI"
    $args += "/D_CRT_SECURE_NO_WARNINGS"
    $args += "/D_CRT_NONSTDC_NO_DEPRECATE"
    if ($HAS_SSL) { $args += "/DMBCRYPT_HAS_OPENSSL" }

    $args += "/I`"$phpInc`""
    $args += "/I`"$phpInc\main`""
    $args += "/I`"$phpInc\Zend`""
    $args += "/I`"$phpInc\TSRM`""
    $args += "/I`"$phpInc\ext`""
    $args += "/I`"$ZLIB_INCL`""
    $args += "/I`"$MSVC_INCL`""
    $args += "/I`"$WINSDK_INC\ucrt`""
    $args += "/I`"$WINSDK_INC\shared`""
    $args += "/I`"$WINSDK_INC\um`""
    if ($HAS_SSL) { $args += "/I`"$SSL_INCL`"" }

    $args += "`"$SRC1`""
    $args += "`"$SRC2`""
    $args += "`"$SRC3`""

    $args += "/link"
    $args += "/DLL"
    $args += "/MACHINE:X64"
    $args += "/INCREMENTAL:NO"
    $args += "/OUT:`"$outDll`""

    $args += "/LIBPATH:`"$MSVC_LIB`""
    $args += "/LIBPATH:`"$WINSDK_LIB\um\x64`""
    $args += "/LIBPATH:`"$WINSDK_LIB\ucrt\x64`""

    $args += "`"$phpLib`""
    $args += "`"$ZLIB_LIB`""
    $args += "kernel32.lib"; $args += "user32.lib"; $args += "ws2_32.lib"; $args += "advapi32.lib"
    if ($HAS_SSL) {
        $args += "`"$SSL_LDIR\libssl.lib`""
        $args += "`"$SSL_LDIR\libcrypto.lib`""
    }

    Write-Host "    Compiling..." -ForegroundColor Gray
    $output = & $CL @args 2>&1
    $code   = $LASTEXITCODE

    Remove-Item "*.obj" -Force -ErrorAction SilentlyContinue
    Remove-Item "*.exp" -Force -ErrorAction SilentlyContinue
    Remove-Item "mbcryptload.lib" -Force -ErrorAction SilentlyContinue
    Remove-Item "mbcrypt_lib.lib" -Force -ErrorAction SilentlyContinue
    Remove-Item "mbpak_lib.lib" -Force -ErrorAction SilentlyContinue

    if ($code -ne 0) {
        Log-Fail "Compilation failed (exit $code)"
        $output | ForEach-Object { Write-Host "    $_" -ForegroundColor DarkGray }
        return $false
    }

    Log-OK "DLL: $outDll"
    return $true
}

# ---------------------------------------------------------------------------
# Main loop
# ---------------------------------------------------------------------------
$results = @{}

foreach ($entry in $MATRIX) {
    foreach ($mode in $THREAD_MODES) {
        $key = "$($entry.FullVer)-$mode"
        $dd = Get-DevelPack -FullVer $entry.FullVer -Mode $mode
        if ($dd -eq $null) {
            $results[$key] = "SKIP (download failed)"
            continue
        }
        $ok = Build-Ext -Entry $entry -Mode $mode -DevelDir $dd
        $results[$key] = if ($ok) { "OK" } else { "FAIL" }
    }
}

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
Log-Step "Summary"
foreach ($key in ($results.Keys | Sort-Object)) {
    $val = $results[$key]
    $color = if ($val -eq "OK") { "Green" } else { "Red" }
    Write-Host ("    PHP {0} : {1}" -f $key, $val) -ForegroundColor $color
}

Write-Host ""
Write-Host "DLLs in: $DIST_DIR"
Get-ChildItem $DIST_DIR -Filter "*.dll" -ErrorAction SilentlyContinue | ForEach-Object {
    $kb = [math]::Round($_.Length / 1024, 1)
    Write-Host "    $($_.Name)  ($kb KB)"
}
