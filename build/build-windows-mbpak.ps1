# build-windows-mbpak.ps1 - Compile mbpak.exe (standalone .mbpak archive CLI) for Windows
# Author: CISSE Mohamed B
# Usage: powershell.exe -ExecutionPolicy Bypass -File build-windows-mbpak.ps1
#
# mbpak.exe has no dependency on any PHP version -- it only needs zlib and
# OpenSSL (mandatory: unlike mbcrypt, .mbpak archives are AES-256-GCM only,
# see mbpak/Makefile). One build is enough for every target PHP version.
# Reuses the same VS2019 (vs16) toolchain as the mbcrypt.exe CLI build in
# build-windows.ps1, for consistency.

$ProgressPreference = "SilentlyContinue"

# ---------------------------------------------------------------------------
# Paths
# ---------------------------------------------------------------------------
$SCRIPT_DIR  = Split-Path -Parent $MyInvocation.MyCommand.Path
$PROJECT_DIR = Split-Path -Parent $SCRIPT_DIR
$DIST_DIR    = Join-Path $SCRIPT_DIR "dist"
$CLI_EXE     = Join-Path $DIST_DIR "mbpak.exe"

$SRC1 = Join-Path $PROJECT_DIR "mbpak\mbpak_cli.c"
$SRC2 = Join-Path $PROJECT_DIR "mbpak\mbpak_lib.c"
$SRC3 = Join-Path $PROJECT_DIR "mbcrypt\mbcrypt_lib.c"   # shared AES-256-GCM/zlib primitives
$INC_MBPAK   = Join-Path $PROJECT_DIR "mbpak"
$INC_MBCRYPT = Join-Path $PROJECT_DIR "mbcrypt"

# VS 2019 BuildTools (vs16)
$VS_MSVC   = "C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\VC\Tools\MSVC\14.29.30133"
$CL_EXE    = "$VS_MSVC\bin\Hostx64\x64\cl.exe"
$MSVC_INCL = "$VS_MSVC\include"
$MSVC_LIB  = "$VS_MSVC\lib\x64"

# Windows SDK
$WINSDK_VER  = "10.0.22621.0"
$WINSDK_ROOT = "C:\Program Files (x86)\Windows Kits\10"
$WINSDK_INC  = "$WINSDK_ROOT\Include\$WINSDK_VER"
$WINSDK_LIB  = "$WINSDK_ROOT\Lib\$WINSDK_VER"

# zlib (Laragon Apache VS16 - compatible VS2019)
$ZLIB_INCL = "C:\laragon\bin\apache\httpd-2.4.54-win64-VS16\include"
$ZLIB_LIB  = "C:\laragon\bin\apache\httpd-2.4.54-win64-VS16\lib\zlib.lib"

# OpenSSL Win64 (mandatory for mbpak -- no legacy/XOR fallback)
$SSL_INCL = "C:\Program Files\OpenSSL-Win64\include"
$SSL_LDIR = "C:\Program Files\OpenSSL-Win64\lib\VC\x64\MD"

function Log-Step { param($m) Write-Host "`n==> $m" -ForegroundColor Cyan }
function Log-OK   { param($m) Write-Host "    [OK] $m" -ForegroundColor Green }
function Log-Warn { param($m) Write-Host "    [!!] $m" -ForegroundColor Yellow }
function Log-Fail { param($m) Write-Host "    [KO] $m" -ForegroundColor Red }

# ---------------------------------------------------------------------------
# Pre-checks
# ---------------------------------------------------------------------------
Log-Step "Environment check"

if (-not (Test-Path $CL_EXE)) { Log-Fail "cl.exe not found: $CL_EXE"; exit 1 }
Log-OK "cl.exe VS2019: $CL_EXE"

if (-not (Test-Path $ZLIB_LIB)) { Log-Fail "zlib.lib not found: $ZLIB_LIB"; exit 1 }
Log-OK "zlib: $ZLIB_INCL"

if (-not (Test-Path "$SSL_INCL\openssl\evp.h")) {
    Log-Fail "OpenSSL not found: $SSL_INCL (mbpak requires OpenSSL -- no legacy mode)"
    exit 1
}
Log-OK "OpenSSL: $SSL_INCL"

if (-not (Test-Path $DIST_DIR)) { New-Item -ItemType Directory -Path $DIST_DIR | Out-Null }

# ---------------------------------------------------------------------------
# Compile
# ---------------------------------------------------------------------------
Log-Step "Compiling mbpak.exe (CLI archive tool)"

$args = @()

$args += "/Fe`"$CLI_EXE`""
$args += "/MD"; $args += "/Ox"; $args += "/W3"; $args += "/nologo"

$args += "/DWIN32"; $args += "/D_WINDOWS"; $args += "/DWIN64"
$args += "/DZLIB_WINAPI"
$args += "/D_CRT_SECURE_NO_WARNINGS"
$args += "/D_CRT_NONSTDC_NO_DEPRECATE"
$args += "/DMBCRYPT_HAS_OPENSSL"

$args += "/I`"$INC_MBPAK`""
$args += "/I`"$INC_MBCRYPT`""
$args += "/I`"$ZLIB_INCL`""
$args += "/I`"$SSL_INCL`""
$args += "/I`"$MSVC_INCL`""
$args += "/I`"$WINSDK_INC\ucrt`""
$args += "/I`"$WINSDK_INC\shared`""
$args += "/I`"$WINSDK_INC\um`""

$args += "`"$SRC1`""
$args += "`"$SRC2`""
$args += "`"$SRC3`""

$args += "/link"
$args += "/MACHINE:X64"
$args += "/INCREMENTAL:NO"
$args += "/SUBSYSTEM:CONSOLE"

$args += "/LIBPATH:`"$MSVC_LIB`""
$args += "/LIBPATH:`"$WINSDK_LIB\um\x64`""
$args += "/LIBPATH:`"$WINSDK_LIB\ucrt\x64`""

$args += "`"$ZLIB_LIB`""
$args += "`"$SSL_LDIR\libssl.lib`""
$args += "`"$SSL_LDIR\libcrypto.lib`""
$args += "kernel32.lib"; $args += "user32.lib"; $args += "advapi32.lib"

Write-Host "    Compiling..." -ForegroundColor Gray
$output = & $CL_EXE @args 2>&1
$code   = $LASTEXITCODE

Remove-Item "*.obj" -Force -ErrorAction SilentlyContinue
Remove-Item "mbpak.lib" -Force -ErrorAction SilentlyContinue
Remove-Item "mbpak.exp" -Force -ErrorAction SilentlyContinue

if ($code -ne 0) {
    Log-Fail "Compilation failed (exit $code)"
    $output | ForEach-Object { Write-Host "    $_" -ForegroundColor DarkGray }
    exit 1
}

Log-OK "EXE: $CLI_EXE"

$exeItem = Get-Item $CLI_EXE
$kb = [math]::Round($exeItem.Length / 1024, 1)
Write-Host ""
Write-Host "mbpak.exe built: $CLI_EXE ($kb KB)"
Write-Host ""
Write-Host "Usage:"
Write-Host "    mbpak.exe pack    -p PASSWORD -o ARCHIVE.mbpak SOURCE_DIR"
Write-Host "    mbpak.exe list    -p PASSWORD ARCHIVE.mbpak"
Write-Host "    mbpak.exe extract -p PASSWORD -o OUT_DIR ARCHIVE.mbpak [VPATH]"
Write-Host "    mbpak.exe put     -p PASSWORD ARCHIVE.mbpak VPATH LOCAL_FILE"
Write-Host "    mbpak.exe get     -p PASSWORD ARCHIVE.mbpak VPATH [-o LOCAL_FILE]"
Write-Host "    mbpak.exe rm      -p PASSWORD ARCHIVE.mbpak VPATH"
Write-Host "    mbpak.exe compact -p PASSWORD ARCHIVE.mbpak"
