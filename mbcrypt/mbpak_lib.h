/*
   +----------------------------------------------------------------------+
   | mbpak - Read/write encrypted archive format (built on mbcrypt)      |
   +----------------------------------------------------------------------+
   | Author  : CISSE Mohamed B <mohamedbachir.cisse@gmail.com>           |
   +----------------------------------------------------------------------+
   | No PHP dependencies — uses standard malloc/free only, exactly like  |
   | mbcrypt_lib. Every file record is a self-contained "MB_ENC_V3" blob |
   | produced by mbcrypt_lib_encode()/mbcrypt_lib_decode() from          |
   | ../mbcrypt/mbcrypt_lib.c — this library adds no new cryptography,   |
   | only the container format around it.                                |
   +----------------------------------------------------------------------+
*/
#ifndef MBPAK_LIB_H
#define MBPAK_LIB_H

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

/* ------------------------------------------------------------------
 * On-disk format (see mbpak/FORMAT.md for the full rationale)
 *
 *   [HEADER  40 bytes, cleartext, fixed offset 0 -- rewritten in place
 *             at every commit, never moves]
 *   [RECORD] [RECORD] ...   -- one per file, append-only, never mutated
 *   [RECORD] -- the TOC itself, appended just like any file record
 *
 * HEADER layout:
 *   [8]  magic        "MBPAK001"
 *   [1]  version       = 3
 *   [3]  reserved      zero-padded
 *   [16] kdf_salt      PBKDF2 salt for the WHOLE archive (see below)
 *   [8]  toc_offset    u64 LE -- byte offset of the current TOC record
 *   [4]  header_crc32  CRC32 of bytes [0..36) -- catches a torn header
 *                      write; NOT a secret, just corruption detection
 *
 * RECORD layout (used for both files and the TOC):
 *   [8]  blob_len  u64 LE
 *   [blob_len] bytes -- raw output of mbcrypt_lib_encode_with_key(),
 *              i.e. an "MB_ENC_V3" blob, AES-256-GCM only (see
 *              FORMAT.md for why legacy XOR is not offered here)
 *
 * TOC plaintext (before it is handed to mbcrypt_lib_encode_with_key()):
 *   [4]  entry_count  u32 LE
 *   repeated entry_count times:
 *     [2]  path_len    u16 LE
 *     [path_len] path  UTF-8, '/'-separated, no leading '/'
 *     [8]  offset      u64 LE -- start of this file's RECORD
 *     [4]  flags       u32 LE -- bit0 = MBPAK_FLAG_TOMBSTONE
 *     [8]  mtime       u64 LE -- unix timestamp, informational only
 *     [8]  size        u64 LE -- plaintext byte length
 *
 * A file's record length is NOT duplicated in the TOC: it is read from
 * the record's own 8-byte prefix at `offset`. One source of truth.
 *
 * `size` IS duplicated on purpose: it is the plaintext length, which
 * cannot be read from the record without a full AES-GCM decrypt +
 * zlib inflate. The mbpak:// stream wrapper's url_stat() (see
 * ../mbcrypt/mbcryptload.c) needs it to answer file_exists()/
 * filesize() cheaply -- Composer's PSR-4 autoloader calls file_exists()
 * on every candidate path while resolving a class, so paying a full
 * decrypt per stat() would be a real cost, not just a purity concern.
 *
 * KEY DERIVATION (v3): `kdf_salt` in the HEADER is used to derive the
 * archive's AES key ONCE, at mbpak_open()/mbpak_create() time, via
 * mbcrypt_lib_derive_key() (PBKDF2-SHA256, ~100ms by design). That one
 * derived key is then reused for every RECORD via
 * mbcrypt_lib_encode_with_key()/decode_with_key() -- each record still
 * gets a fresh random GCM nonce (required for AES-GCM safety) and
 * still carries its own (now-unused) salt field for blob-format
 * consistency, but no longer re-derives the key from the password.
 * v1/v2 derived a fresh key from the password for EVERY record
 * (mbcrypt_lib_encode()/decode()), which meant N files cost N x the
 * PBKDF2 tax -- ~100ms x N, discovered while packing a real Laravel
 * app/ directory (see pistes-a-explorer.md). v3 pays that cost exactly
 * once per mount, regardless of how many files are read afterward.
 * ------------------------------------------------------------------ */

#define MBPAK_MAGIC       "MBPAK001"
#define MBPAK_MAGIC_LEN   8
#define MBPAK_VERSION     3
#define MBPAK_HEADER_LEN  40
#define MBPAK_KDF_SALT_LEN 16 /* == MBCRYPT_PBKDF2_SALT_LEN, kept literal
                                * here so this header does not need to
                                * include mbcrypt_lib.h just for one
                                * constant (mbpak_lib.c asserts they
                                * match at compile time via mbcrypt_lib.h). */

#define MBPAK_FLAG_TOMBSTONE  0x1u

typedef struct mbpak_entry {
    char                *path;   /* owned, NUL-terminated                */
    uint64_t             offset; /* byte offset of this file's RECORD    */
    uint32_t             flags;  /* MBPAK_FLAG_*                         */
    uint64_t             mtime;  /* unix timestamp, informational        */
    uint64_t             size;   /* plaintext byte length                */
    struct mbpak_entry  *next;
} mbpak_entry_t;

typedef struct {
    FILE          *fp;
    char          *path;         /* archive file path, owned copy        */
    char          *password;     /* owned copy, required (AES-GCM only)  */
    uint8_t        kdf_salt[MBPAK_KDF_SALT_LEN]; /* fixed for archive lifetime */
    uint8_t        key[32];      /* derived once from password+kdf_salt  */
    uint64_t       toc_offset;
    mbpak_entry_t *entries;      /* in-memory TOC, includes tombstones   */
    int            dirty;        /* 1 if entries changed since last flush */
} mbpak_t;

/* Create a brand-new, empty archive at `path`. Fails (returns NULL) if
 * `path` already exists, or on I/O / encryption failure. The archive is
 * left in a valid, mountable state immediately (empty TOC committed). */
mbpak_t *mbpak_create(const char *path, const char *password);

/* Open an existing archive for read/write. Decrypts and parses the TOC
 * immediately, so a wrong password fails right here (GCM auth failure)
 * rather than later on individual files. */
mbpak_t *mbpak_open(const char *path, const char *password);

/* Insert or replace the file at `vpath`. Appends a new encrypted record
 * at end-of-file and updates the in-memory TOC only; no existing record
 * is touched. Call mbpak_flush() to commit the TOC + header to disk. */
int mbpak_put(mbpak_t *pak, const char *vpath, const char *data, size_t len);

/* Read and decrypt the file at `vpath`. Returns a malloc'd buffer the
 * caller must free(), or NULL if not found, tombstoned, or the record
 * fails GCM authentication (tampered data / wrong password). */
char *mbpak_get(mbpak_t *pak, const char *vpath, size_t *out_len);

/* Mark `vpath` as deleted (tombstone). Bytes stay in the archive until
 * mbpak_compact(). Returns 0 if the path existed and was live, -1
 * otherwise. */
int mbpak_delete(mbpak_t *pak, const char *vpath);

/* In-memory TOC head (read-only view) -- walk ->next; entries with
 * MBPAK_FLAG_TOMBSTONE set are deleted and should usually be skipped. */
const mbpak_entry_t *mbpak_list(const mbpak_t *pak);

/* Serialize the in-memory TOC, encrypt it, append it, then rewrite the
 * 24-byte header in place to point at it. This header rewrite is the
 * *only* in-place disk write and the single commit point: a crash
 * before it completes leaves the archive at its previous, still-valid
 * state (old TOC, old header, untouched). No-op if nothing changed. */
int mbpak_flush(mbpak_t *pak);

/* Rewrite the archive keeping only live records, reclaiming space used
 * by tombstoned or superseded (overwritten) entries. Flushes pending
 * changes first. On success, `pak` keeps pointing at the same logical
 * archive (compacted in place via a temp file + rename). Returns 0 on
 * success; on failure the original archive is left untouched. */
int mbpak_compact(mbpak_t *pak);

/* Flush pending changes (best-effort), close the file and free `pak`. */
void mbpak_close(mbpak_t *pak);

#endif /* MBPAK_LIB_H */
