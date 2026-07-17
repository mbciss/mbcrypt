# build-windows-static.ps1
# Compile PHP CLI avec mbcryptload integre en STATIQUE
# PHP 8.1 / 8.2 / 8.3 -- VS2019 x64
# Author: CISSE Mohamed Bassirou
# Usage : powershell.exe -ExecutionPolicy Bypass -File build-windows-static.ps1

$ProgressPreference = "SilentlyContinue"

# ---------------------------------------------------------------------------
# Paths
# ---------------------------------------------------------------------------
$SCRIPT_DIR  = Split-Path -Parent $MyInvocation.MyCommand.Path
$PROJECT_DIR = Split-Path -Parent $SCRIPT_DIR
$EXT_SRC     = Join-Path $PROJECT_DIR "mbcrypt"

$SDK_DIR     = Join-Path $SCRIPT_DIR "php-sdk-tools"
$BUILDS_DIR  = Join-Path $SCRIPT_DIR "php-static-builds"
$DIST_DIR    = Join-Path $SCRIPT_DIR "dist\static"

# VS2019
$VCVARSALL = "C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvarsall.bat"

# PHP versions to build
$PHP_BUILDS = @(
    @{ Ver="8.1"; FullVer="8.1.10"; Branch="8.1" },
    @{ Ver="8.2"; FullVer="8.2.15"; Branch="8.2" },
    @{ Ver="8.3"; FullVer="8.3.16"; Branch="8.3" }
)

# Extensions to include in the static build
# mbcryptload=static : notre extension compilee en statique dans php.exe
# opcache : ne peut pas etre statique, compile en shared (.dll)
# Les autres : statiques
$CONFIGURE_OPTS = @(
    "--disable-all",
    "--enable-cli",
    "--enable-mbcryptload=static",
    "--with-zlib",
    "--with-openssl",
    "--enable-bcmath",
    "--enable-mbstring",
    "--enable-mbregex",
    "--enable-opcache",
    "--enable-pdo",
    "--with-sqlite3",
    "--with-pdo-sqlite",
    "--enable-ctype",
    "--enable-filter",
    "--enable-hash",
    "--enable-tokenizer",
    "--enable-sockets",
    "--enable-fileinfo",
    "--with-curl"
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

if (-not (Test-Path $VCVARSALL)) {
    Log-Fail "vcvarsall.bat not found: $VCVARSALL"
    Write-Host "    Install Visual Studio 2019 Build Tools (C++ workload)" -ForegroundColor Yellow
    exit 1
}
Log-OK "VS2019 vcvarsall: $VCVARSALL"

if (-not (Test-Path $BUILDS_DIR)) { New-Item -ItemType Directory -Path $BUILDS_DIR | Out-Null }
if (-not (Test-Path $DIST_DIR))   { New-Item -ItemType Directory -Path $DIST_DIR   | Out-Null }

# ---------------------------------------------------------------------------
# Step 1 -- Download PHP SDK binary tools
# ---------------------------------------------------------------------------
Log-Step "PHP SDK binary tools"

$SDK_MARKER = Join-Path $SDK_DIR "phpsdk-vc16-x64.bat"

if (Test-Path $SDK_MARKER) {
    Log-OK "SDK already present: $SDK_DIR"
} else {
    Write-Host "    Fetching latest release from GitHub..." -ForegroundColor Gray
    try {
        $release = Invoke-RestMethod -Uri "https://api.github.com/repos/php/php-sdk-binary-tools/releases/latest" -UseBasicParsing
        $asset   = $release.assets | Where-Object { $_.name -like "*.zip" } | Select-Object -First 1
        if (-not $asset) {
            # Fallback: use archive of master branch
            $zipUrl = "https://github.com/php/php-sdk-binary-tools/archive/refs/heads/master.zip"
        } else {
            $zipUrl = $asset.browser_download_url
        }
    } catch {
        $zipUrl = "https://github.com/php/php-sdk-binary-tools/archive/refs/heads/master.zip"
    }

    $sdkZip = Join-Path $SCRIPT_DIR "php-sdk-tools.zip"
    Write-Host "    Downloading: $zipUrl" -ForegroundColor Gray

    try {
        Invoke-WebRequest -Uri $zipUrl -OutFile $sdkZip -UseBasicParsing -TimeoutSec 120
        Expand-Archive -Path $sdkZip -DestinationPath $SCRIPT_DIR -Force

        # Rename extracted folder (may be php-sdk-binary-tools-master)
        $extracted = Get-ChildItem $SCRIPT_DIR -Directory | Where-Object {
            $_.Name -like "php-sdk-binary-tools*"
        } | Select-Object -First 1

        if ($extracted -and $extracted.FullName -ne $SDK_DIR) {
            Rename-Item $extracted.FullName $SDK_DIR -Force
        }

        Remove-Item $sdkZip -Force -ErrorAction SilentlyContinue
        Log-OK "SDK extracted: $SDK_DIR"
    } catch {
        Log-Fail "Failed to download PHP SDK: $_"
        exit 1
    }
}

# ---------------------------------------------------------------------------
# Step 2 -- Download PHP source + build each version
# ---------------------------------------------------------------------------
function Get-PHPSource {
    param($FullVer)

    $name = "php-$FullVer-src"
    $zip  = Join-Path $BUILDS_DIR "$name.zip"
    $dest = Join-Path $BUILDS_DIR $FullVer

    if (Test-Path (Join-Path $dest "buildconf.bat")) {
        Log-OK "Source $FullVer already extracted."
        return $dest
    }

    $urls = @(
        "https://windows.php.net/downloads/releases/php-$FullVer-src.zip",
        "https://windows.php.net/downloads/releases/archives/php-$FullVer-src.zip",
        "https://github.com/php/php-src/archive/refs/tags/php-$FullVer.zip"
    )

    $got = $false
    foreach ($url in $urls) {
        Write-Host "    Trying source: $url" -ForegroundColor Gray
        try {
            Invoke-WebRequest -Uri $url -OutFile $zip -UseBasicParsing -TimeoutSec 300
            $got = $true
            Log-OK "Downloaded source $FullVer"
            break
        } catch {
            Log-Warn "Failed: $url"
        }
    }

    if (-not $got) {
        Log-Fail "Cannot download PHP $FullVer source"
        return $null
    }

    if (-not (Test-Path $dest)) { New-Item -ItemType Directory -Path $dest | Out-Null }
    Expand-Archive -Path $zip -DestinationPath $dest -Force

    # Flatten inner folder if present
    $inner = Get-ChildItem $dest -Directory | Select-Object -First 1
    if ($inner -and -not (Test-Path (Join-Path $dest "buildconf.bat"))) {
        Get-ChildItem $inner.FullName | ForEach-Object {
            Move-Item $_.FullName $dest -Force
        }
        Remove-Item $inner.FullName -Recurse -Force -ErrorAction SilentlyContinue
    }

    Remove-Item $zip -Force -ErrorAction SilentlyContinue
    return $dest
}

function Build-Static {
    param($Build, $SrcDir)

    $ver     = $Build.Ver
    $fullVer = $Build.FullVer
    $branch  = $Build.Branch

    Log-Step "PHP $fullVer -- static build (mbcryptload + modules)"

    # --- Copy our extension into the PHP source tree ---
    $extDest = Join-Path $SrcDir "ext\mbcryptload"
    if (-not (Test-Path $extDest)) { New-Item -ItemType Directory -Path $extDest | Out-Null }

    Copy-Item (Join-Path $EXT_SRC "mbcryptload.c")     $extDest -Force
    Copy-Item (Join-Path $EXT_SRC "mbcrypt_lib.c")     $extDest -Force
    Copy-Item (Join-Path $EXT_SRC "mbcrypt_lib.h")     $extDest -Force
    Copy-Item (Join-Path $EXT_SRC "php_mbcryptload.h") $extDest -Force
    Copy-Item (Join-Path $EXT_SRC "config.w32")        $extDest -Force
    Log-OK "Extension sources copied to $extDest"

    # --- Deps directory (SDK downloads here) ---
    $depsDir = Join-Path $BUILDS_DIR "deps-$ver"
    if (-not (Test-Path $depsDir)) { New-Item -ItemType Directory -Path $depsDir | Out-Null }

    # --- Output directory ---
    $outDir = Join-Path $DIST_DIR "php-$ver"
    if (-not (Test-Path $outDir)) { New-Item -ItemType Directory -Path $outDir | Out-Null }

    # --- Configure options string ---
    $confOpts = ($CONFIGURE_OPTS | ForEach-Object { $_ }) -join " "
    $confOpts += " --with-php-build=`"$depsDir`""
    $confOpts += " --without-php-build-deps"

    # --- Write the build batch file ---
    $batchFile = Join-Path $BUILDS_DIR "build_php${ver}.bat"
    $releaseDir = Join-Path $SrcDir "x64\Release_TS"

    $batch = @"
@echo off
setlocal

REM Setup VS2019 x64 environment
call "$VCVARSALL" amd64
if errorlevel 1 ( echo [KO] vcvarsall failed & exit /b 1 )

REM Add PHP SDK tools to PATH
set "PATH=$SDK_DIR\bin\vc16\x64;$SDK_DIR\bin;%PATH%"
set "PHPSDKDIR=$SDK_DIR"

REM Download dependencies for this PHP branch
echo [..] Downloading deps for PHP $branch ...
cd /d "$SDK_DIR"
call phpsdk_deps.bat --branch $branch --deps "$depsDir" --x64
if errorlevel 1 ( echo [!!] phpsdk_deps failed -- continuing without update & )

REM Build PHP
cd /d "$SrcDir"
echo [..] Running buildconf ...
call buildconf.bat --force
if errorlevel 1 ( echo [KO] buildconf failed & exit /b 1 )

echo [..] Running configure ...
cscript /nologo configure.js $confOpts
if errorlevel 1 ( echo [KO] configure failed & exit /b 1 )

echo [..] Running nmake ...
nmake
if errorlevel 1 ( echo [KO] nmake failed & exit /b 1 )

echo [OK] Build complete.
exit /b 0
"@

    [System.IO.File]::WriteAllText($batchFile, $batch, [System.Text.Encoding]::ASCII)

    # --- Run the build ---
    Write-Host "    Building (this can take 10-20 minutes)..." -ForegroundColor Gray
    $proc = Start-Process -FilePath "cmd.exe" `
        -ArgumentList "/c `"$batchFile`" 2>&1" `
        -WorkingDirectory $SrcDir `
        -Wait -PassThru `
        -RedirectStandardOutput (Join-Path $BUILDS_DIR "build_php${ver}.log") `
        -RedirectStandardError  (Join-Path $BUILDS_DIR "build_php${ver}_err.log")

    $code = $proc.ExitCode

    if ($code -ne 0) {
        Log-Fail "Build failed (exit $code). See log:"
        Write-Host "    $BUILDS_DIR\build_php${ver}.log" -ForegroundColor DarkGray
        Write-Host "    $BUILDS_DIR\build_php${ver}_err.log" -ForegroundColor DarkGray

        # Show last 30 lines of log
        $log = Join-Path $BUILDS_DIR "build_php${ver}.log"
        if (Test-Path $log) {
            Write-Host "`n    --- Last build output ---" -ForegroundColor DarkGray
            Get-Content $log -Tail 30 | ForEach-Object { Write-Host "    $_" -ForegroundColor DarkGray }
        }
        return $false
    }

    # --- Collect outputs ---
    # PHP build outputs to x64\Release_TS (ZTS) or x64\Release (NTS)
    # We build NTS-compatible (--disable-all disables ZTS by default unless --enable-zts)
    $releaseDirs = @(
        (Join-Path $SrcDir "x64\Release_TS"),
        (Join-Path $SrcDir "x64\Release"),
        (Join-Path $SrcDir "Release_TS"),
        (Join-Path $SrcDir "Release")
    )

    $phpExe = $null
    foreach ($rd in $releaseDirs) {
        $candidate = Join-Path $rd "php.exe"
        if (Test-Path $candidate) { $phpExe = $candidate; break }
    }

    if (-not $phpExe) {
        Log-Fail "php.exe not found after build"
        return $false
    }

    $releaseDir = Split-Path $phpExe -Parent
    Write-Host "    Collecting output from: $releaseDir" -ForegroundColor Gray

    # Copy php.exe
    Copy-Item $phpExe (Join-Path $outDir "php.exe") -Force

    # Copy dependency DLLs (from release dir, not subdirs)
    Get-ChildItem $releaseDir -Filter "*.dll" -File | ForEach-Object {
        Copy-Item $_.FullName $outDir -Force
    }

    # Copy ext/ folder (shared extensions like opcache)
    $extOutDir = Join-Path $releaseDir "ext"
    if (Test-Path $extOutDir) {
        $extDist = Join-Path $outDir "ext"
        if (-not (Test-Path $extDist)) { New-Item -ItemType Directory -Path $extDist | Out-Null }
        Get-ChildItem $extOutDir -Filter "*.dll" | ForEach-Object {
            Copy-Item $_.FullName $extDist -Force
        }
    }

    # Generate a minimal php.ini
    $phpIni = @"
; php.ini generated by build-windows-static.ps1
; mbcryptload is compiled STATICALLY -- no extension= line needed

; Set your AES decryption password here:
; mbcryptload.password = "your-password-here"

; Opcache (loaded as shared)
zend_extension=php_opcache.dll
opcache.enable=1
opcache.enable_cli=1

; Extension directory
extension_dir=ext
"@
    [System.IO.File]::WriteAllText((Join-Path $outDir "php.ini"), $phpIni, [System.Text.Encoding]::ASCII)

    $phpSize = [math]::Round((Get-Item $phpExe).Length / 1024)
    Log-OK "php.exe: $outDir\php.exe ($phpSize KB)"
    return $true
}

# ---------------------------------------------------------------------------
# Main loop
# ---------------------------------------------------------------------------
$results = @{}

foreach ($build in $PHP_BUILDS) {
    $srcDir = Get-PHPSource -FullVer $build.FullVer
    if (-not $srcDir) {
        $results[$build.FullVer] = "SKIP (source unavailable)"
        continue
    }
    $ok = Build-Static -Build $build -SrcDir $srcDir
    $results[$build.FullVer] = if ($ok) { "OK" } else { "FAIL" }
}

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
Log-Step "Summary"
foreach ($kv in $results.GetEnumerator()) {
    $color = if ($kv.Value -eq "OK") { "Green" } else { "Red" }
    Write-Host ("    PHP {0} : {1}" -f $kv.Key, $kv.Value) -ForegroundColor $color
}

Write-Host ""
Write-Host "Static builds in: $DIST_DIR"
Get-ChildItem $DIST_DIR -Directory -ErrorAction SilentlyContinue | ForEach-Object {
    $phpExe = Join-Path $_.FullName "php.exe"
    if (Test-Path $phpExe) {
        $kb = [math]::Round((Get-Item $phpExe).Length / 1024)
        Write-Host "    $($_.Name)\php.exe  ($kb KB)"
    }
}

Write-Host ""
Write-Host "Usage:"
Write-Host "    dist\static\php-8.3\php.exe --ri mbcryptload"
Write-Host "    dist\static\php-8.3\php.exe fichier_chiffre.php"
Write-Host ""
Write-Host "mbcryptload.password dans php.ini :"
Write-Host "    mbcryptload.password = `"votre-mot-de-passe`""
