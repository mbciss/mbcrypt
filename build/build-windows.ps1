# build-windows.ps1 - Compile mbcryptload.dll for PHP 8.1 / 8.2 / 8.3
# Author: CISSE Mohamed B
# Usage: powershell.exe -ExecutionPolicy Bypass -File build-windows.ps1

$ProgressPreference = "SilentlyContinue"

# ---------------------------------------------------------------------------
# Paths
# ---------------------------------------------------------------------------
$SCRIPT_DIR  = Split-Path -Parent $MyInvocation.MyCommand.Path
$PROJECT_DIR = Split-Path -Parent $SCRIPT_DIR
$DIST_DIR    = Join-Path $SCRIPT_DIR "dist"
$DEVEL_DIR   = Join-Path $SCRIPT_DIR "devel-packs"

$SRC1     = Join-Path $PROJECT_DIR "mbcrypt\mbcryptload.c"
$SRC2     = Join-Path $PROJECT_DIR "mbcrypt\mbcrypt_lib.c"
$SRC3     = Join-Path $PROJECT_DIR "mbcrypt\mbpak_lib.c"   # .mbpak archive support, always linked into mbcryptload.c
$SRC_CLI1 = Join-Path $PROJECT_DIR "mbcrypt\mbcrypt.c"
$SRC_CLI2 = Join-Path $PROJECT_DIR "mbcrypt\mbcrypt_lib.c"
$CLI_EXE  = Join-Path $DIST_DIR    "mbcrypt.exe"

# VS 2019 BuildTools
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

# OpenSSL Win64
$SSL_INCL = "C:\Program Files\OpenSSL-Win64\include"
$SSL_LDIR = "C:\Program Files\OpenSSL-Win64\lib\VC\x64\MD"

# PHP installations in Laragon
$PHP_BUILDS = @(
    @{ Ver="81"; FullVer="8.1.10"; Dir="C:\laragon\bin\php\php-8.1.10-Win32-vs16-x64" },
    @{ Ver="82"; FullVer="8.2.15"; Dir="C:\laragon\bin\php\php-8.2.15-Win32-vs16-x64" },
    @{ Ver="83"; FullVer="8.3.16"; Dir="C:\laragon\bin\php\php-8.3.16-Win32-vs16-x64" }
)

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------
function Log-Step { param($m) Write-Host "`n==> $m" -ForegroundColor Cyan }
function Log-OK   { param($m) Write-Host "    [OK] $m" -ForegroundColor Green }
function Log-Warn { param($m) Write-Host "    [!!] $m" -ForegroundColor Yellow }
function Log-Fail { param($m) Write-Host "    [KO] $m" -ForegroundColor Red }

# ---------------------------------------------------------------------------
# Pre-checks
# ---------------------------------------------------------------------------
Log-Step "Environment check"

if (-not (Test-Path $CL_EXE)) {
    Log-Fail "cl.exe not found: $CL_EXE"
    exit 1
}
Log-OK "cl.exe VS2019: $CL_EXE"

if (-not (Test-Path $ZLIB_LIB)) {
    Log-Fail "zlib.lib not found: $ZLIB_LIB"
    exit 1
}
Log-OK "zlib: $ZLIB_INCL"

$HAS_SSL = Test-Path "$SSL_INCL\openssl\evp.h"
if ($HAS_SSL) {
    Log-OK "OpenSSL: $SSL_INCL"
} else {
    Log-Warn "OpenSSL not found -- AES-256-GCM disabled (XOR only)"
}

if (-not (Test-Path $DEVEL_DIR)) { New-Item -ItemType Directory -Path $DEVEL_DIR | Out-Null }
if (-not (Test-Path $DIST_DIR))  { New-Item -ItemType Directory -Path $DIST_DIR  | Out-Null }

# ---------------------------------------------------------------------------
# Download devel packs
# ---------------------------------------------------------------------------
Log-Step "Downloading PHP devel packs"

function Get-DevalPack {
    param($FullVer)
    $name  = "php-devel-pack-$FullVer-Win32-vs16-x64"
    $zip   = Join-Path $DEVEL_DIR "$name.zip"
    $dest  = Join-Path $DEVEL_DIR $FullVer
    $incl  = Join-Path $dest "include"

    if (Test-Path $incl) {
        Log-OK "Devel pack $FullVer already extracted."
        return $dest
    }

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
        Log-Fail "Cannot download devel pack $FullVer"
        Write-Host "    Download manually from https://windows.php.net/download/" -ForegroundColor Yellow
        Write-Host "    and extract to: $dest" -ForegroundColor Yellow
        return $null
    }

    if (-not (Test-Path $dest)) { New-Item -ItemType Directory -Path $dest | Out-Null }
    Expand-Archive -Path $zip -DestinationPath $dest -Force

    # Flatten: move contents of inner folder up
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
# Compile one PHP version
# ---------------------------------------------------------------------------
function Build-Ext {
    param($Build, $DevalDir)

    $ver    = $Build.Ver
    $phpDir = $Build.Dir
    $phpLib = "$phpDir\dev\php8ts.lib"
    $outDll = Join-Path $DIST_DIR "php_mbcryptload_php$ver`_ts.dll"

    Log-Step "Compiling PHP $($Build.FullVer) ZTS"

    if (-not (Test-Path $phpLib)) {
        Log-Fail "php8ts.lib missing: $phpLib"
        return $false
    }

    $phpInc = Join-Path $DevalDir "include"
    if (-not (Test-Path $phpInc)) {
        Log-Fail "PHP headers missing in: $DevalDir"
        return $false
    }

    # Build argument list
    $args = @()

    # Compile flags
    $args += "/LD"
    $args += "/MD"
    $args += "/Ox"
    $args += "/W3"
    $args += "/nologo"

    # Defines
    $args += "/DWIN32"
    $args += "/D_WINDOWS"
    $args += "/DWIN64"
    $args += "/DPHP_WIN32"
    $args += "/DZEND_WIN32"
    $args += "/DZTS"
    $args += "/DUSING_ZTS=1"
    $args += "/DZEND_DEBUG=0"
    $args += "/DCOMPILE_DL_MBCRYPTLOAD"
    $args += "/DZLIB_WINAPI"
    $args += "/D_CRT_SECURE_NO_WARNINGS"
    $args += "/D_CRT_NONSTDC_NO_DEPRECATE"
    if ($HAS_SSL) { $args += "/DMBCRYPT_HAS_OPENSSL" }

    # Includes
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

    # Source files
    $args += "`"$SRC1`""
    $args += "`"$SRC2`""
    $args += "`"$SRC3`""

    # Linker
    $args += "/link"
    $args += "/DLL"
    $args += "/MACHINE:X64"
    $args += "/INCREMENTAL:NO"
    $args += "/OUT:`"$outDll`""

    # Library search paths (let linker auto-resolve CRT from these)
    $args += "/LIBPATH:`"$MSVC_LIB`""
    $args += "/LIBPATH:`"$WINSDK_LIB\um\x64`""
    $args += "/LIBPATH:`"$WINSDK_LIB\ucrt\x64`""

    # Explicit import libraries
    $args += "`"$phpLib`""
    $args += "`"$ZLIB_LIB`""
    $args += "kernel32.lib"
    $args += "user32.lib"
    $args += "ws2_32.lib"
    $args += "advapi32.lib"
    if ($HAS_SSL) {
        $args += "`"$SSL_LDIR\libssl.lib`""
        $args += "`"$SSL_LDIR\libcrypto.lib`""
    }

    Write-Host "    Compiling..." -ForegroundColor Gray
    $output = & $CL_EXE @args 2>&1
    $code   = $LASTEXITCODE

    # Clean temp object files
    Remove-Item "*.obj" -Force -ErrorAction SilentlyContinue
    Remove-Item "*.exp" -Force -ErrorAction SilentlyContinue
    # Don't remove *.lib - might delete zlib.lib in current dir
    Remove-Item "mbcryptload.lib" -Force -ErrorAction SilentlyContinue
    Remove-Item "mbcrypt_lib.lib" -Force -ErrorAction SilentlyContinue
    Remove-Item "mbpak_lib.lib" -Force -ErrorAction SilentlyContinue

    if ($code -ne 0) {
        Log-Fail "Compilation failed (exit $code)"
        $output | ForEach-Object { Write-Host "    $_" -ForegroundColor DarkGray }
        return $false
    }

    Log-OK "DLL: $outDll"

    # Auto-install into Laragon ext/
    $extDir = "$phpDir\ext"
    if (Test-Path $extDir) {
        $target = Join-Path $extDir "php_mbcryptload.dll"
        Copy-Item $outDll $target -Force
        Log-OK "Installed in Laragon: $target"
    }

    return $true
}

# ---------------------------------------------------------------------------
# Build CLI encoder (mbcrypt.exe)
# ---------------------------------------------------------------------------
function Build-CLI {
    Log-Step "Compiling mbcrypt.exe (CLI encoder)"

    $args = @()

    # Compile + link as console EXE
    $args += "/Fe`"$CLI_EXE`""
    $args += "/MD"
    $args += "/Ox"
    $args += "/W3"
    $args += "/nologo"

    # Defines
    $args += "/DWIN32"
    $args += "/D_WINDOWS"
    $args += "/DWIN64"
    $args += "/DZLIB_WINAPI"
    $args += "/D_CRT_SECURE_NO_WARNINGS"
    $args += "/D_CRT_NONSTDC_NO_DEPRECATE"
    if ($HAS_SSL) { $args += "/DMBCRYPT_HAS_OPENSSL" }

    # Includes
    $args += "/I`"$ZLIB_INCL`""
    $args += "/I`"$MSVC_INCL`""
    $args += "/I`"$WINSDK_INC\ucrt`""
    $args += "/I`"$WINSDK_INC\shared`""
    $args += "/I`"$WINSDK_INC\um`""
    if ($HAS_SSL) { $args += "/I`"$SSL_INCL`"" }

    # Source files
    $args += "`"$SRC_CLI1`""
    $args += "`"$SRC_CLI2`""

    # Linker
    $args += "/link"
    $args += "/MACHINE:X64"
    $args += "/INCREMENTAL:NO"
    $args += "/SUBSYSTEM:CONSOLE"

    # Library search paths
    $args += "/LIBPATH:`"$MSVC_LIB`""
    $args += "/LIBPATH:`"$WINSDK_LIB\um\x64`""
    $args += "/LIBPATH:`"$WINSDK_LIB\ucrt\x64`""

    # Explicit import libraries
    $args += "`"$ZLIB_LIB`""
    $args += "kernel32.lib"
    $args += "user32.lib"
    if ($HAS_SSL) {
        $args += "`"$SSL_LDIR\libssl.lib`""
        $args += "`"$SSL_LDIR\libcrypto.lib`""
    }

    Write-Host "    Compiling..." -ForegroundColor Gray
    $output = & $CL_EXE @args 2>&1
    $code   = $LASTEXITCODE

    Remove-Item "*.obj" -Force -ErrorAction SilentlyContinue
    Remove-Item "*.exp" -Force -ErrorAction SilentlyContinue
    Remove-Item "mbcrypt.lib" -Force -ErrorAction SilentlyContinue
    Remove-Item "mbcrypt_lib.lib" -Force -ErrorAction SilentlyContinue

    if ($code -ne 0) {
        Log-Fail "CLI compilation failed (exit $code)"
        $output | ForEach-Object { Write-Host "    $_" -ForegroundColor DarkGray }
        return $false
    }

    Log-OK "EXE: $CLI_EXE"
    return $true
}

# ---------------------------------------------------------------------------
# Main loop
# ---------------------------------------------------------------------------
$results = @{}

foreach ($build in $PHP_BUILDS) {
    $dd = Get-DevalPack -FullVer $build.FullVer
    if ($dd -eq $null) {
        $results[$build.FullVer] = "SKIP"
        continue
    }
    $ok = Build-Ext -Build $build -DevalDir $dd
    if ($ok) {
        $results[$build.FullVer] = "OK"
    } else {
        $results[$build.FullVer] = "FAIL"
    }
}

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
Log-Step "Summary"
foreach ($kv in $results.GetEnumerator()) {
    if ($kv.Value -eq "OK") {
        Write-Host ("    PHP {0} : {1}" -f $kv.Key, $kv.Value) -ForegroundColor Green
    } else {
        Write-Host ("    PHP {0} : {1}" -f $kv.Key, $kv.Value) -ForegroundColor Red
    }
}

Write-Host ""
Write-Host "DLLs in: $DIST_DIR"
Get-ChildItem $DIST_DIR -Filter "*.dll" -ErrorAction SilentlyContinue | ForEach-Object {
    $kb = [math]::Round($_.Length / 1024, 1)
    Write-Host "    $($_.Name)  ($kb KB)"
}

Write-Host ""
Write-Host "To enable the extension, add to php.ini:"
Write-Host "    extension=php_mbcryptload"

# ---------------------------------------------------------------------------
# Build CLI
# ---------------------------------------------------------------------------
$cli_ok = Build-CLI

Write-Host ""
Write-Host "CLI encoder in: $DIST_DIR"
if ($cli_ok) {
    $exeItem = Get-Item $CLI_EXE -ErrorAction SilentlyContinue
    if ($exeItem) {
        $kb = [math]::Round($exeItem.Length / 1024, 1)
        Write-Host "    $($exeItem.Name)  ($kb KB)"
    }
    Write-Host ""
    Write-Host "Usage:"
    Write-Host "    mbcrypt -o output.php source.php"
    Write-Host "    mbcrypt -p password -o output.php source.php"
    Write-Host "    mbcrypt -r -o output_dir/ source_dir/"
    Write-Host "    mbcrypt -r -p password -o output_dir/ source_dir/"
}
