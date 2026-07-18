/*
   +----------------------------------------------------------------------+
   | mbpak - Read/write encrypted archive format (built on mbcrypt)      |
   +----------------------------------------------------------------------+
   | Author  : CISSE Mohamed B <mohamedbachir.cisse@gmail.com>           |
   +----------------------------------------------------------------------+
*/
#include "mbpak_lib.h"
#include "mbcrypt_lib.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <zlib.h>   /* crc32() -- already a hard dependency via mbcrypt_lib */

/* ==================================================================
 * Small portable helpers
 * ================================================================== */

static char *xstrdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *p = (char *)malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

/* Fixed-endianness (LE) integer encode/decode -- the on-disk format must
 * not depend on host byte order (project targets x86_64/ARM/MIPS). */
static void put_u16le(uint8_t *p, uint16_t v) { p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); }
static void put_u32le(uint8_t *p, uint32_t v) { p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); p[2]=(uint8_t)(v>>16); p[3]=(uint8_t)(v>>24); }
static void put_u64le(uint8_t *p, uint64_t v) { int i; for (i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8*i)); }

static uint16_t get_u16le(const uint8_t *p) { return (uint16_t)(p[0] | (p[1]<<8)); }
static uint32_t get_u32le(const uint8_t *p) { return (uint32_t)(p[0] | (p[1]<<8) | (p[2]<<16) | ((uint32_t)p[3]<<24)); }
static uint64_t get_u64le(const uint8_t *p)
{
    uint64_t v = 0; int i;
    for (i = 7; i >= 0; i--) v = (v << 8) | p[i];
    return v;
}

static void free_entries(mbpak_entry_t *e)
{
    while (e) { mbpak_entry_t *n = e->next; free(e->path); free(e); e = n; }
}

/* ==================================================================
 * Header (24 bytes, cleartext, fixed offset 0)
 * ================================================================== */

static uint32_t header_crc(const uint8_t *hdr20)
{
    return (uint32_t)crc32(0L, hdr20, 20);
}

static int write_header(FILE *fp, uint64_t toc_offset)
{
    uint8_t hdr[MBPAK_HEADER_LEN];

    memcpy(hdr, MBPAK_MAGIC, MBPAK_MAGIC_LEN);
    hdr[8] = MBPAK_VERSION;
    hdr[9] = hdr[10] = hdr[11] = 0;
    put_u64le(hdr + 12, toc_offset);
    put_u32le(hdr + 20, header_crc(hdr));

    if (fseek(fp, 0, SEEK_SET) != 0) return -1;
    if (fwrite(hdr, 1, MBPAK_HEADER_LEN, fp) != MBPAK_HEADER_LEN) return -1;
    if (fflush(fp) != 0) return -1;
    return 0;
}

static int read_header(FILE *fp, uint64_t *toc_offset)
{
    uint8_t hdr[MBPAK_HEADER_LEN];

    if (fseek(fp, 0, SEEK_SET) != 0) return -1;
    if (fread(hdr, 1, MBPAK_HEADER_LEN, fp) != MBPAK_HEADER_LEN) return -1;
    if (memcmp(hdr, MBPAK_MAGIC, MBPAK_MAGIC_LEN) != 0) return -1;
    if (hdr[8] != MBPAK_VERSION) return -1;
    if (header_crc(hdr) != get_u32le(hdr + 20)) return -1; /* corrupted header */

    *toc_offset = get_u64le(hdr + 12);
    return 0;
}

/* ==================================================================
 * Records: [8-byte LE length][blob] -- self-describing, append-only
 * ================================================================== */

/* Caller must have nothing else pending at EOF; this always appends. */
static int write_record(FILE *fp, const char *blob, size_t blob_len,
                         uint64_t *out_offset)
{
    uint8_t lenbuf[8];
    long pos;

    if (fseek(fp, 0, SEEK_END) != 0) return -1;
    pos = ftell(fp);
    if (pos < 0) return -1;

    put_u64le(lenbuf, (uint64_t)blob_len);
    if (fwrite(lenbuf, 1, 8, fp) != 8) return -1;
    if (blob_len && fwrite(blob, 1, blob_len, fp) != blob_len) return -1;
    if (fflush(fp) != 0) return -1;

    *out_offset = (uint64_t)pos;
    return 0;
}

/* 1 GiB sanity cap on a single record -- guards against a corrupted or
 * truncated length prefix causing a runaway allocation. */
#define MBPAK_MAX_RECORD ((uint64_t)0x40000000)

static char *read_record(FILE *fp, uint64_t offset, size_t *out_len)
{
    uint8_t lenbuf[8];
    uint64_t blob_len;
    char *buf;

    if (fseek(fp, (long)offset, SEEK_SET) != 0) return NULL;
    if (fread(lenbuf, 1, 8, fp) != 8) return NULL;

    blob_len = get_u64le(lenbuf);
    if (blob_len == 0 || blob_len > MBPAK_MAX_RECORD) return NULL;

    buf = (char *)malloc((size_t)blob_len);
    if (!buf) return NULL;
    if (fread(buf, 1, (size_t)blob_len, fp) != blob_len) { free(buf); return NULL; }

    *out_len = (size_t)blob_len;
    return buf;
}

/* ==================================================================
 * TOC (de)serialization -- plaintext form, encrypted as a RECORD
 * ================================================================== */

static char *serialize_toc(const mbpak_entry_t *entries, size_t *out_len)
{
    const mbpak_entry_t *e;
    uint32_t count = 0;
    size_t total = 4;
    char *buf, *p;

    for (e = entries; e; e = e->next) {
        total += 2 + strlen(e->path) + 8 + 4 + 8 + 8;
        count++;
    }

    buf = (char *)malloc(total); /* always >= 4, count==0 still valid */
    if (!buf) return NULL;

    p = buf;
    put_u32le((uint8_t *)p, count); p += 4;
    for (e = entries; e; e = e->next) {
        size_t plen = strlen(e->path);
        put_u16le((uint8_t *)p, (uint16_t)plen); p += 2;
        memcpy(p, e->path, plen); p += plen;
        put_u64le((uint8_t *)p, e->offset); p += 8;
        put_u32le((uint8_t *)p, e->flags);  p += 4;
        put_u64le((uint8_t *)p, e->mtime);  p += 8;
        put_u64le((uint8_t *)p, e->size);   p += 8;
    }

    *out_len = total;
    return buf;
}

/* Returns 0 and sets *out (possibly NULL for an empty TOC) on success.
 * Returns -1 on malformed input, leaving *out untouched -- this should
 * not happen in practice since the buffer just passed GCM authentication
 * in mbcrypt_lib_decode(), but the bounds are still checked defensively
 * against a parsing bug rather than trusted blindly. A bare NULL return
 * would not let the caller tell "empty archive" apart from "corrupt". */
static int parse_toc(const char *buf, size_t len, mbpak_entry_t **out)
{
    const uint8_t *p = (const uint8_t *)buf;
    const uint8_t *end = p + len;
    uint32_t count, i;
    mbpak_entry_t *head = NULL, *tail = NULL;

    if (len < 4) return -1;
    count = get_u32le(p); p += 4;

    for (i = 0; i < count; i++) {
        mbpak_entry_t *e;
        uint16_t plen;

        if (end - p < 2) goto fail;
        plen = get_u16le(p); p += 2;

        if (end - p < (ptrdiff_t)plen + 28) goto fail;

        e = (mbpak_entry_t *)calloc(1, sizeof(mbpak_entry_t));
        if (!e) goto fail;

        e->path = (char *)malloc((size_t)plen + 1);
        if (!e->path) { free(e); goto fail; }
        memcpy(e->path, p, plen); e->path[plen] = '\0'; p += plen;

        e->offset = get_u64le(p); p += 8;
        e->flags  = get_u32le(p); p += 4;
        e->mtime  = get_u64le(p); p += 8;
        e->size   = get_u64le(p); p += 8;

        if (tail) tail->next = e; else head = e;
        tail = e;
    }
    *out = head;
    return 0;

fail:
    free_entries(head);
    return -1;
}

/* ==================================================================
 * Load the TOC pointed to by the header into pak->entries
 * ================================================================== */

static int load_toc(mbpak_t *pak)
{
    char *toc_blob, *toc_plain;
    size_t toc_blob_len, toc_plain_len;
    mbpak_entry_t *entries;
    int rc;

    if (read_header(pak->fp, &pak->toc_offset) != 0) return -1;

    toc_blob = read_record(pak->fp, pak->toc_offset, &toc_blob_len);
    if (!toc_blob) return -1;

    toc_plain = mbcrypt_lib_decode(toc_blob, toc_blob_len, &toc_plain_len,
                                    pak->password);
    free(toc_blob);
    if (!toc_plain) return -1; /* wrong password or corrupted TOC */

    rc = parse_toc(toc_plain, toc_plain_len, &entries);
    free(toc_plain);
    if (rc != 0) return -1;

    pak->entries = entries;
    return 0;
}

/* ==================================================================
 * Public API
 * ================================================================== */

mbpak_t *mbpak_create(const char *path, const char *password)
{
    mbpak_t *pak;
    FILE *fp, *probe;

    if (!path || !password || !*password) return NULL;

    probe = fopen(path, "rb");
    if (probe) { fclose(probe); return NULL; } /* refuse to clobber */

    fp = fopen(path, "w+b");
    if (!fp) return NULL;

    pak = (mbpak_t *)calloc(1, sizeof(mbpak_t));
    if (!pak) { fclose(fp); return NULL; }

    pak->fp       = fp;
    pak->path     = xstrdup(path);
    pak->password = xstrdup(password);
    pak->dirty    = 1; /* forces an initial empty-TOC commit below */

    /* Reserve the 24-byte header slot *before* the first record is ever
     * appended. Without this, write_record()'s "append at EOF" would
     * land at offset 0 on a brand-new empty file, and the header write
     * that follows in mbpak_flush() would then clobber that record --
     * toc_offset==0 pointing right back into the header itself. */
    if (!pak->path || !pak->password || write_header(fp, 0) != 0 ||
        mbpak_flush(pak) != 0) {
        mbpak_close(pak);
        remove(path);
        return NULL;
    }
    return pak;
}

mbpak_t *mbpak_open(const char *path, const char *password)
{
    mbpak_t *pak;
    FILE *fp;

    if (!path || !password || !*password) return NULL;

    fp = fopen(path, "r+b");
    if (!fp) return NULL;

    pak = (mbpak_t *)calloc(1, sizeof(mbpak_t));
    if (!pak) { fclose(fp); return NULL; }

    pak->fp       = fp;
    pak->path     = xstrdup(path);
    pak->password = xstrdup(password);

    if (!pak->path || !pak->password || load_toc(pak) != 0) {
        mbpak_close(pak);
        return NULL;
    }
    return pak;
}

int mbpak_put(mbpak_t *pak, const char *vpath, const char *data, size_t len)
{
    char *blob;
    size_t blob_len;
    uint64_t offset;
    mbpak_entry_t *e;

    if (!pak || !vpath || !*vpath || !data || !len) return -1;

    blob = mbcrypt_lib_encode(data, len, &blob_len, MBCRYPT_AES_GCM_MODE,
                               pak->password);
    if (!blob) return -1;

    if (write_record(pak->fp, blob, blob_len, &offset) != 0) {
        free(blob);
        return -1;
    }
    free(blob);

    for (e = pak->entries; e; e = e->next) {
        if (strcmp(e->path, vpath) == 0) {
            e->offset = offset;
            e->flags &= ~MBPAK_FLAG_TOMBSTONE;
            e->mtime  = (uint64_t)time(NULL);
            e->size   = (uint64_t)len;
            pak->dirty = 1;
            return 0;
        }
    }

    e = (mbpak_entry_t *)calloc(1, sizeof(mbpak_entry_t));
    if (!e) return -1;
    e->path = xstrdup(vpath);
    if (!e->path) { free(e); return -1; }
    e->offset = offset;
    e->mtime  = (uint64_t)time(NULL);
    e->size   = (uint64_t)len;
    e->next   = pak->entries;
    pak->entries = e;
    pak->dirty = 1;
    return 0;
}

char *mbpak_get(mbpak_t *pak, const char *vpath, size_t *out_len)
{
    mbpak_entry_t *e;
    char *blob, *plain;
    size_t blob_len;

    if (out_len) *out_len = 0;
    if (!pak || !vpath) return NULL;

    for (e = pak->entries; e; e = e->next)
        if (strcmp(e->path, vpath) == 0) break;

    if (!e || (e->flags & MBPAK_FLAG_TOMBSTONE)) return NULL;

    blob = read_record(pak->fp, e->offset, &blob_len);
    if (!blob) return NULL;

    plain = mbcrypt_lib_decode(blob, blob_len, out_len, pak->password);
    free(blob);
    return plain;
}

int mbpak_delete(mbpak_t *pak, const char *vpath)
{
    mbpak_entry_t *e;

    if (!pak || !vpath) return -1;
    for (e = pak->entries; e; e = e->next) {
        if (strcmp(e->path, vpath) == 0 && !(e->flags & MBPAK_FLAG_TOMBSTONE)) {
            e->flags |= MBPAK_FLAG_TOMBSTONE;
            pak->dirty = 1;
            return 0;
        }
    }
    return -1;
}

const mbpak_entry_t *mbpak_list(const mbpak_t *pak)
{
    return pak ? pak->entries : NULL;
}

int mbpak_flush(mbpak_t *pak)
{
    char *toc_plain, *toc_blob;
    size_t toc_plain_len, toc_blob_len;
    uint64_t new_toc_offset;

    if (!pak) return -1;
    if (!pak->dirty) return 0;

    toc_plain = serialize_toc(pak->entries, &toc_plain_len);
    if (!toc_plain) return -1;

    /* serialize_toc() always emits >= 4 bytes (entry_count), so this
     * never hits mbcrypt_lib_encode()'s "empty input" rejection even
     * for a brand-new, empty archive. */
    toc_blob = mbcrypt_lib_encode(toc_plain, toc_plain_len, &toc_blob_len,
                                   MBCRYPT_AES_GCM_MODE, pak->password);
    free(toc_plain);
    if (!toc_blob) return -1;

    if (write_record(pak->fp, toc_blob, toc_blob_len, &new_toc_offset) != 0) {
        free(toc_blob);
        return -1;
    }
    free(toc_blob);

    if (write_header(pak->fp, new_toc_offset) != 0) return -1;

    pak->toc_offset = new_toc_offset;
    pak->dirty = 0;
    return 0;
}

int mbpak_compact(mbpak_t *pak)
{
    char tmp_path[4096];
    mbpak_t *tmp;
    mbpak_entry_t *e;

    if (!pak) return -1;
    if (mbpak_flush(pak) != 0) return -1;

    if (snprintf(tmp_path, sizeof(tmp_path), "%s.compact.tmp", pak->path)
        >= (int)sizeof(tmp_path)) return -1;
    remove(tmp_path); /* leftover from a previous failed attempt */

    tmp = mbpak_create(tmp_path, pak->password);
    if (!tmp) return -1;

    for (e = pak->entries; e; e = e->next) {
        char *data; size_t len; int ok;

        if (e->flags & MBPAK_FLAG_TOMBSTONE) continue;

        data = mbpak_get(pak, e->path, &len);
        ok = data && mbpak_put(tmp, e->path, data, len) == 0;
        free(data);
        if (!ok) { mbpak_close(tmp); remove(tmp_path); return -1; }
    }

    if (mbpak_flush(tmp) != 0) { mbpak_close(tmp); remove(tmp_path); return -1; }
    mbpak_close(tmp); /* the compacted archive is complete and valid on disk */

    fclose(pak->fp);
    pak->fp = NULL;
    if (rename(tmp_path, pak->path) != 0) {
        pak->fp = fopen(pak->path, "r+b"); /* rename failed: original untouched */
        return -1;
    }

    free_entries(pak->entries);
    pak->entries = NULL;

    pak->fp = fopen(pak->path, "r+b");
    if (!pak->fp || load_toc(pak) != 0) return -1;

    pak->dirty = 0;
    return 0;
}

void mbpak_close(mbpak_t *pak)
{
    if (!pak) return;
    if (pak->dirty) mbpak_flush(pak); /* best-effort commit */
    if (pak->fp) fclose(pak->fp);
    free_entries(pak->entries);
    free(pak->path);
    free(pak->password);
    free(pak);
}
