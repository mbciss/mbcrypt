/*
   +----------------------------------------------------------------------+
   | mbcrypt - Secure PHP file encryption library                        |
   +----------------------------------------------------------------------+
   | Author  : CISSE Mohamed B <mohamedbachir.cisse@gmail.com>           |
   | Original: sun bing <hoowa.sun@gmail.com>                            |
   +----------------------------------------------------------------------+
   | License : GPL-2.0                                                    |
   +----------------------------------------------------------------------+
*/
#ifndef MBCRYPT_LIB_H
#define MBCRYPT_LIB_H

#include <stdint.h>
#include <stddef.h>

/* ------------------------------------------------------------------
 * File format tags
 * ------------------------------------------------------------------ */
#define MBCRYPTTAG_V2_STR   "MB_ENC_V2"   /* Legacy tag (read-only) */
#define MBCRYPTTAG_V3_STR   "MB_ENC_V3"   /* Current tag            */
#define MBCRYPTTAG_LEN      9

/* ------------------------------------------------------------------
 * Legacy XOR key — regenerate with ./init_key
 * ------------------------------------------------------------------ */
#define MBCRYPTKEY 18447,763,892,7347,26797,24482,978,62,464,18,600,061,138,1321,1,1342

/* ------------------------------------------------------------------
 * Compression
 * ------------------------------------------------------------------ */
#define ZENCOMPRESS   0
#define ZDECOMPRESS   1
#define ZLIB_CHUNK    65536   /* Streaming chunk size (heap) */

/* ------------------------------------------------------------------
 * Encryption modes
 * ------------------------------------------------------------------ */
#define MBCRYPT_LEGACY_MODE   0   /* XOR (backward compatible) */
#define MBCRYPT_AES_GCM_MODE  1   /* AES-256-GCM               */

/* ------------------------------------------------------------------
 * AES-256-GCM parameters
 * ------------------------------------------------------------------ */
#define MBCRYPT_AES_KEY_LEN         32   /* 256 bits   */
#define MBCRYPT_AES_IV_LEN          12   /* 96-bit GCM nonce */
#define MBCRYPT_AES_TAG_LEN         16   /* 128-bit GCM auth tag */
#define MBCRYPT_PBKDF2_SALT_LEN     16   /* 128-bit PBKDF2 salt  */
#define MBCRYPT_PBKDF2_ITER     100000   /* PBKDF2-SHA256 iterations */

/* ------------------------------------------------------------------
 * V3 binary header (packed, immediately at start of encrypted blob)
 *
 * Layout:
 *   [9]  tag       "MB_ENC_V3"
 *   [1]  version   = 3
 *   [1]  method    MBCRYPT_LEGACY_MODE | MBCRYPT_AES_GCM_MODE
 *   [6]  reserved  (zero-padded)
 *   --- if method == AES_GCM, immediately after header: ---
 *   [16] salt      PBKDF2 salt
 *   [12] iv        GCM nonce
 *   [16] gcm_tag   GCM authentication tag
 *   --- then: ciphertext (= zlib-compressed plaintext, encrypted) ---
 * ------------------------------------------------------------------ */
#pragma pack(push, 1)
typedef struct {
    uint8_t tag[MBCRYPTTAG_LEN]; /* "MB_ENC_V3"                    */
    uint8_t version;              /* = 3                            */
    uint8_t method;               /* MBCRYPT_LEGACY_MODE or AES_GCM */
    uint8_t reserved[6];          /* zero-padded, future use        */
} mbcrypt_v3_header_t;
#pragma pack(pop)

#define MBCRYPT_V3_BASE_LEN  ((size_t)sizeof(mbcrypt_v3_header_t))       /* 17 bytes */
#define MBCRYPT_V3_AES_META  (MBCRYPT_PBKDF2_SALT_LEN + MBCRYPT_AES_IV_LEN + MBCRYPT_AES_TAG_LEN) /* 44 bytes */
#define MBCRYPT_V3_AES_LEN   (MBCRYPT_V3_BASE_LEN + MBCRYPT_V3_AES_META) /* 61 bytes */

/* ------------------------------------------------------------------
 * Sentinel return value for mbcrypt_lib_detect()
 * ------------------------------------------------------------------ */
#define MBCRYPT_NOT_ENCRYPTED  (-1)

/* ------------------------------------------------------------------
 * Public API — all buffers use standard malloc/free (no PHP deps)
 * ------------------------------------------------------------------ */

/* In-place XOR cipher (symmetric — same function for enc and dec).
 * Key and scramble are controlled by ./init_key.                  */
void  mbcrypt_lib_xor(char *buf, size_t len);

/* Zlib single-pass compress (compressBound, one malloc).
 * Returns malloc'd buffer; *out_len set on success.
 * Returns NULL on error. Caller must free().                       */
char *mbcrypt_lib_zcompress(const char *in, size_t in_len, size_t *out_len);

/* Zlib streaming decompress (heap chunks, no stack buffers).
 * Returns malloc'd buffer; *out_len set on success.
 * Returns NULL on error. Caller must free().                       */
char *mbcrypt_lib_zdecompress(const char *in, size_t in_len, size_t *out_len);

/* Detect whether a blob is an mbcrypt-encrypted file.
 * Returns MBCRYPT_LEGACY_MODE, MBCRYPT_AES_GCM_MODE,
 * or MBCRYPT_NOT_ENCRYPTED if unrecognised.                        */
int   mbcrypt_lib_detect(const char *blob, size_t blob_len);

/* High-level: compress then encrypt → V3 blob.
 * mode:     MBCRYPT_LEGACY_MODE or MBCRYPT_AES_GCM_MODE
 * password: required for AES mode; ignored / may be NULL for legacy.
 * Returns malloc'd V3 blob; caller must free().
 * Returns NULL on error.                                           */
char *mbcrypt_lib_encode(const char *src,  size_t src_len,  size_t *out_len,
                          int mode, const char *password);

/* High-level: decrypt + decompress V3 (or V2) blob → plaintext PHP.
 * password: required for AES files; may be NULL for legacy.
 * Returns NULL on auth failure, bad format, or alloc error.
 * Returns malloc'd plaintext; caller must free().                  */
char *mbcrypt_lib_decode(const char *blob, size_t blob_len, size_t *out_len,
                          const char *password);

/* ------------------------------------------------------------------
 * Bulk / pre-derived-key API — for a caller managing many small blobs
 * under ONE password (e.g. an archive format) that wants to pay the
 * PBKDF2 cost once instead of once per blob. AES-256-GCM (mbpak) only
 * — there is no legacy-XOR equivalent. Requires MBCRYPT_HAS_OPENSSL
 * at build time; all four return an error (NULL / -1) otherwise.
 * ------------------------------------------------------------------ */

/* Fill `salt` with MBCRYPT_PBKDF2_SALT_LEN random bytes.
 * Returns 0 on success, -1 on error.                                 */
int mbcrypt_lib_random_salt(uint8_t salt[MBCRYPT_PBKDF2_SALT_LEN]);

/* Derive a 256-bit AES key from `password` + `salt` (PBKDF2-SHA256,
 * MBCRYPT_PBKDF2_ITER iterations — the same ~100ms cost paid inside
 * one mbcrypt_lib_encode()/decode() call, paid here exactly once for
 * however many mbcrypt_lib_*_with_key() blobs follow).
 * Returns 0 on success, -1 on error.                                 */
int mbcrypt_lib_derive_key(const char *password,
                            const uint8_t salt[MBCRYPT_PBKDF2_SALT_LEN],
                            uint8_t key[MBCRYPT_AES_KEY_LEN]);

/* Same output format as mbcrypt_lib_encode(..., MBCRYPT_AES_GCM_MODE, ..)
 * — a "MB_ENC_V3" AES blob — but takes an already-derived key instead
 * of a password, skipping PBKDF2 entirely. The blob's own salt field
 * is still present for structural/format consistency (filled with
 * random bytes) but plays no role in mbcrypt_lib_decode_with_key().
 * Returns malloc'd V3 blob; caller must free(). Returns NULL on error. */
char *mbcrypt_lib_encode_with_key(const char *src, size_t src_len, size_t *out_len,
                                   const uint8_t key[MBCRYPT_AES_KEY_LEN]);

/* Counterpart to mbcrypt_lib_encode_with_key(): decrypt + decompress a
 * V3 AES blob using an already-derived key instead of a password.
 * Returns NULL on GCM authentication failure (tampered data / wrong
 * key) or bad format. Returns malloc'd plaintext; caller must free(). */
char *mbcrypt_lib_decode_with_key(const char *blob, size_t blob_len, size_t *out_len,
                                   const uint8_t key[MBCRYPT_AES_KEY_LEN]);

#endif /* MBCRYPT_LIB_H */
