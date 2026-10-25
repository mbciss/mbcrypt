# Security Policy

## Supported versions

| Version | Supported |
|---------|-----------|
| 3.x     | Yes       |
| < 3.0 (V2 / XOR) | No, legacy decrypt only |

## Reporting a vulnerability

Please **do not open a public issue** for security problems.
Use GitHub's private reporting: *Security > Report a vulnerability* on this repository,
or write to mohamedbachir.cisse@gmail.com.

You will get an acknowledgement within 7 days.

## Scope and limits

MBCRYPT raises the cost of reading PHP source; it cannot make code impossible to recover
by someone who controls the machine running it (the decrypted opcodes live in process memory).
The legacy XOR mode is obfuscation, not encryption. Use AES-256-GCM mode for anything sensitive.