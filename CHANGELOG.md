# Changelog

## 3.0.0
- AES-256-GCM with PBKDF2-SHA256 (100 000 iterations), per-file salt and nonce
- Legacy XOR mode and V2 decryption kept for compatibility
- Heap-only zlib, thread-safe (NTS/ZTS), PHP 7.0 to 8.5
- `.mbpak` archives and `mbpak://` stream wrapper, key derived once per archive
- Windows (12 DLLs, PHP 8.0-8.5 NTS/TS) and Linux (12 .so) builds