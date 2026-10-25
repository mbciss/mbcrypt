# MBCRYPT v3

> **Documentation:** https://kalya-technologies.com/mbcrypt  
> **Binaries** (Windows DLL/EXE, Linux .so): see [Releases](https://github.com/mbciss/mbcrypt/releases), verify with `SHA256SUMS.txt`
> **License:** GPL-2.0, see [LICENSE](LICENSE) and [NOTICE.md](NOTICE.md). **Security:** [SECURITY.md](SECURITY.md)

MBCRYPT is a PHP script encryption tool. When distributing commercial PHP software, files can be encrypted and are decrypted transparently at runtime by a PHP extension. The original script is never written to disk.

Based on PHP-Screw, rewritten by CISSE Mohamed Bassirou.

## Features

- **Real AES-256-GCM encryption** with PBKDF2-SHA256 key derivation (100 000 iterations, random salt + nonce per file)
- **GCM authentication tag**: any file tampering is detected before execution
- **Legacy XOR mode** kept for backward compatibility with V2-encrypted files
- **Backward-compatible**: V2 files (old format) are still decrypted transparently
- **Heap-only zlib**: no stack buffers, robust for large files, no stack overflow
- **Batch encryption**: encrypt an entire directory tree recursively (`-r`)
- **Thread-safe PHP extension** (ZTS / NTS)
- **PHP 7.0 to 8.x** compatible (including PHP 8.1+ `zend_string*` changes)
- Tested on Ubuntu / CentOS / OpenWRT: x86\_64 / ARM / MIPS
- Supports PHP JIT (PHP 8+)
- **`.mbpak` archives** (read/write, one opaque file for a whole app,
  file names and structure hidden too) via `mbpak_mount()`/the
  `mbpak://` stream wrapper: see [`mbpak/README.md`](mbpak/README.md)

## File format V3

```
[9 bytes]  Tag          "MB_ENC_V3"
[1 byte]   Version      3
[1 byte]   Method       0 = legacy XOR  |  1 = AES-256-GCM
[6 bytes]  Reserved     (zero-padded)
--- AES mode only ---
[16 bytes] PBKDF2 salt
[12 bytes] GCM nonce (IV)
[16 bytes] GCM authentication tag
--- Data ---
[N bytes]  zlib-compressed plaintext, encrypted
```

## Requirements

| Dependency | Role | Notes |
|------------|------|-------|
| zlib       | Compression | Required |
| OpenSSL ≥ 1.1 | AES-256-GCM | Optional. Without it, only XOR mode |

### PHP version support

| PHP Version | Status |
|-------------|--------|
| PHP 8.x     | Recommended |
| PHP 7.4.x   | Supported |
| PHP 7.0 to 7.3 | Supported |

PHP must be compiled with `--with-zlib`.

## Installation

### 1. Generate a unique key (recommended)

```bash
# From the mbcrypt/ directory
perl init_key
# Then recompile both the encoder AND the PHP extension
```

### 2. Build the PHP extension (shared)

```bash
cd mbcrypt/
phpize
./configure --enable-mbcryptload          # OpenSSL auto-detected
make
sudo make install

# Add to php.ini
echo "extension=mbcryptload.so" >> /etc/php/8.x/cli/php.ini

# Verify
php -m | grep mbcryptload
php --ri mbcryptload
```

### 3. Build the encoder CLI

```bash
cd mbcrypt/
make                # auto-detects OpenSSL via pkg-config
sudo make install   # installs to /usr/local/bin/mbcrypt
```

## Usage

### Encrypt a single file

```bash
# Legacy XOR mode (no password required)
mbcrypt -o output.php source.php

# AES-256-GCM mode (requires OpenSSL build)
mbcrypt -p "my-strong-password" -o output.php source.php
```

### Encrypt an entire directory recursively

```bash
# Encrypts all .php files; creates output_dir/ with the same structure
mbcrypt -r -o output_dir/ source_dir/
mbcrypt -r -p "password" -o output_dir/ source_dir/
```

### PHP runtime API

```php
<?php
// Required for AES-encrypted files
mbcryptload_set_password("my-strong-password");

// Encrypt a file from PHP
mbcryptload_encrypt_file("source.php", "output.php");                      // XOR
mbcryptload_encrypt_file("source.php", "output.php", "password", true);   // AES

// Get extension version
echo mbcryptload_version(); // "3.0.0"
```

### Set password via environment variable

```bash
export MBCRYPTLOAD_PASSWORD="my-strong-password"
php script.php
```

## Build PHP extension statically

```bash
# Copy extension into PHP source tree
cp -avf mbcrypt/ ~/php-8.x.y/ext/mbcryptload/
cd ~/php-8.x.y
./buildconf --force
./configure --enable-mbcryptload=static --with-zlib
make && make install
```

## Security notes

- **AES-256-GCM** is authenticated encryption: a wrong password or tampered file returns an error instead of garbage output.
- The **XOR legacy key** is compiled into both the encoder and the extension. Run `init_key` to personalise it before distributing.
- The password for AES files is **never stored on disk**: set it via `mbcryptload_set_password()` or the `MBCRYPTLOAD_PASSWORD` environment variable.
- `show_source()` and `highlight_file()` are blocked on encrypted files.

## Author

CISSE Mohamed Bassirou, <mohamedbachir.cisse@gmail.com>  
Original code: sun bing <hoowa.sun@gmail.com>
