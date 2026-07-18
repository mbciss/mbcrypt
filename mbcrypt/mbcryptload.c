/*
   +----------------------------------------------------------------------+
   | mbcryptload - PHP extension for transparent mbcrypt decryption       |
   +----------------------------------------------------------------------+
   | Author  : CISSE Mohamed B <mohamedbachir.cisse@gmail.com>           |
   | Original: sun bing <hoowa.sun@gmail.com>                            |
   +----------------------------------------------------------------------+
   | Compatible with PHP 7.0 – 8.x (including 8.1+ zend_string changes) |
   | Thread-safe (ZTS) via ZEND_MODULE_GLOBALS.                          |
   +----------------------------------------------------------------------+
*/
#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include "php.h"
#include "php_ini.h"
#include "ext/standard/info.h"
#include "ext/standard/php_filestat.h"
#include "php_streams.h"
#include "php_memory_streams.h"
#include "php_mbcryptload.h"
#include "mbcrypt_lib.h"
#include "mbpak_lib.h"

/* ------------------------------------------------------------------
 * Compat: ZEND_PARSE_PARAMETERS_NONE was added in PHP 7.1
 * ------------------------------------------------------------------ */
#ifndef ZEND_PARSE_PARAMETERS_NONE
#  define ZEND_PARSE_PARAMETERS_NONE() \
     ZEND_PARSE_PARAMETERS_START(0, 0) \
     ZEND_PARSE_PARAMETERS_END()
#endif

/* ==================================================================
 * PHP version compatibility
 * ================================================================== */

/* PHP 8.1+: zend_file_handle.filename is zend_string* (was const char*) */
#if PHP_VERSION_ID >= 80100
#  define MBCRYPT_FH_FILENAME(fh) \
     ((fh)->filename ? ZSTR_VAL((fh)->filename) : NULL)
#else
#  define MBCRYPT_FH_FILENAME(fh) ((fh)->filename)
#endif

/* ==================================================================
 * Thread-safe module globals
 * ================================================================== */
ZEND_BEGIN_MODULE_GLOBALS(mbcryptload)
    char    *password;     /* runtime password set via mbcryptload_set_password() */
    char    *ini_password; /* password set via mbcryptload.password in php.ini    */
    mbpak_t *mounted;      /* archive mounted via mbpak_mount(), or NULL          */
ZEND_END_MODULE_GLOBALS(mbcryptload)

ZEND_DECLARE_MODULE_GLOBALS(mbcryptload)

#ifdef ZTS
#  define MBCRYPTLOAD_G(v) \
     TSRMG(mbcryptload_globals_id, zend_mbcryptload_globals *, v)
#else
#  define MBCRYPTLOAD_G(v) (mbcryptload_globals.v)
#endif

static void mbcryptload_globals_ctor(zend_mbcryptload_globals *g)
{
    g->password     = NULL;
    g->ini_password = NULL;
    g->mounted      = NULL;
}

/* No globals_dtor is registered with ZEND_INIT_MODULE_GLOBALS: on Windows,
 * TSRM invokes it at tsrm_shutdown() time, which for a dynamically loaded
 * (COMPILE_DL) ZTS extension happens after the DLL has already been
 * unloaded — calling back into freed/unmapped code crashes the process.
 * Cleanup is instead done unconditionally from MSHUTDOWN below, which is
 * guaranteed to run while the module is still mapped. */

/* ==================================================================
 * php.ini entries
 * ================================================================== */
PHP_INI_BEGIN()
    STD_PHP_INI_ENTRY("mbcryptload.password", "",
        PHP_INI_SYSTEM,
        OnUpdateString,
        ini_password,
        zend_mbcryptload_globals,
        mbcryptload_globals)
PHP_INI_END()

/* ==================================================================
 * Original compile_file pointer
 * ================================================================== */
static zend_op_array *(*org_compile_file)(zend_file_handle *, int);

/* ==================================================================
 * Internal helpers
 * ================================================================== */

/* Read a file via fseek/ftell (avoids stat/fstat platform issues).
 * Returns a malloc'd buffer; caller must free().                     */
static char *fh_read_file(const char *filename, size_t *out_len)
{
    FILE *fp;
    long fsize;
    char *buf;

    *out_len = 0;
    fp = fopen(filename, "rb");
    if (!fp) return NULL;

    if (fseek(fp, 0, SEEK_END) != 0 || (fsize = ftell(fp)) < 0) {
        fclose(fp);
        return NULL;
    }
    rewind(fp);

    *out_len = (size_t)fsize;
    buf = (char *)malloc(*out_len);
    if (!buf) { fclose(fp); *out_len = 0; return NULL; }

    if (*out_len > 0 && fread(buf, 1, *out_len, fp) != *out_len) {
        fclose(fp);
        free(buf);
        *out_len = 0;
        return NULL;
    }
    fclose(fp);
    return buf;
}

/* Inject decrypted content into a file handle.
 *
 * In all PHP versions (7.x and 8.x), zend_file_handle.buf is char*
 * and zend_file_handle.len is size_t.  Only zend_file_handle.filename
 * changed to zend_string* in PHP 8.1+ (handled by MBCRYPT_FH_FILENAME).
 *
 * After this call, caller must free() its own buffer; the handle now
 * owns a separate PHP-managed (emalloc'd) copy.                      */
static void mbcrypt_inject_buf(zend_file_handle *fh,
                                const char *data, size_t len)
{
    if (fh->buf) {
        efree((char *)fh->buf);
    }
    fh->buf = estrndup(data, len);
    fh->len = len;
    /* Prevent PHP from trying to close a stream we already closed */
    fh->handle.stream.closer = NULL;
}

/* ==================================================================
 * Core compile-file hook
 * ================================================================== */
static zend_op_array *mbcryptload_compile_file(zend_file_handle *file_handle,
                                                int type)
{
    const char *filename;
    char *file_data = NULL, *decrypted = NULL;
    size_t file_len = 0, dec_len = 0;
    const char *password;
    int method;

    filename = MBCRYPT_FH_FILENAME(file_handle);
    if (!filename) return org_compile_file(file_handle, type);

    /* Block show_source() / highlight_file() from exposing encrypted
     * source — let the original handler deal with those calls.       */
    if (EG(current_execute_data)                              &&
        EG(current_execute_data)->func                        &&
        EG(current_execute_data)->func->common.function_name) {
        const char *fn = ZSTR_VAL(
            EG(current_execute_data)->func->common.function_name);
        if (strcasecmp(fn, "show_source")    == 0 ||
            strcasecmp(fn, "highlight_file") == 0) {
            return org_compile_file(file_handle, type);
        }
    }

    /* Read the raw file bytes into a malloc buffer */
    file_data = fh_read_file(filename, &file_len);
    if (!file_data) return org_compile_file(file_handle, type);

    /* Fast header check before doing any cryptographic work */
    method = mbcrypt_lib_detect(file_data, file_len);
    if (method == MBCRYPT_NOT_ENCRYPTED) {
        free(file_data);
        return org_compile_file(file_handle, type);
    }

    /* Password priority: PHP API → php.ini → environment variable */
    password = MBCRYPTLOAD_G(password);
    if (!password || !*password) password = MBCRYPTLOAD_G(ini_password);
    if (!password || !*password) password = getenv("MBCRYPTLOAD_PASSWORD");
    if (!password || !*password) password = NULL;

    /* Decrypt + decompress into a fresh malloc buffer */
    decrypted = mbcrypt_lib_decode(file_data, file_len, &dec_len, password);
    free(file_data);

    if (!decrypted || !dec_len) {
        /* Could not decrypt (wrong/missing password or corrupt file).
         * Fall back so PHP gives a useful error rather than crashing. */
        free(decrypted);
        return org_compile_file(file_handle, type);
    }

    /* Hand the plaintext to PHP — mbcrypt_inject_buf copies it into
     * PHP-managed memory so we can safely free our malloc buffer.    */
    mbcrypt_inject_buf(file_handle, decrypted, dec_len);
    free(decrypted);

    return org_compile_file(file_handle, type);
}

/* ==================================================================
 * PHP userland functions
 * ================================================================== */

/* bool mbcryptload_set_password(string $password)
 * Set the decryption password for AES-encrypted files at runtime.   */
PHP_FUNCTION(mbcryptload_set_password)
{
    char *pass;
    size_t pass_len;

    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_STRING(pass, pass_len)
    ZEND_PARSE_PARAMETERS_END();

    /* Persistent (pemalloc-backed) storage: this field is module-global
     * and must survive past the end of the request that set it — a plain
     * estrndup()/efree() pair is request-scoped and would be silently
     * invalidated by Zend's end-of-request cleanup, corrupting the heap
     * the next time it is freed (e.g. from MSHUTDOWN). */
    if (MBCRYPTLOAD_G(password)) {
        pefree(MBCRYPTLOAD_G(password), 1);
    }
    MBCRYPTLOAD_G(password) = pestrndup(pass, pass_len, 1);
    RETURN_TRUE;
}

/* bool mbcryptload_encrypt_file(
 *         string $source, string $target
 *         [, string $password [, bool $use_aes = false]] )
 * Encrypt a PHP file from within PHP itself.                         */
PHP_FUNCTION(mbcryptload_encrypt_file)
{
    char *src = NULL, *dst = NULL, *pass = NULL;
    size_t src_len = 0, dst_len = 0, pass_len = 0;
    zend_bool use_aes = 0;
    char *src_data = NULL, *enc_data = NULL;
    size_t src_sz = 0, enc_sz = 0;
    int mode;
    FILE *fp;

    ZEND_PARSE_PARAMETERS_START(2, 4)
        Z_PARAM_STRING(src, src_len)
        Z_PARAM_STRING(dst, dst_len)
        Z_PARAM_OPTIONAL
        Z_PARAM_STRING(pass, pass_len)
        Z_PARAM_BOOL(use_aes)
    ZEND_PARSE_PARAMETERS_END();

    mode = (use_aes && pass_len > 0) ? MBCRYPT_AES_GCM_MODE
                                      : MBCRYPT_LEGACY_MODE;

    src_data = fh_read_file(src, &src_sz);
    if (!src_data) {
        php_error_docref(NULL, E_WARNING,
            "mbcryptload_encrypt_file: cannot read '%s'", src);
        RETURN_FALSE;
    }

    /* Guard against double-encryption */
    if (src_sz >= MBCRYPTTAG_LEN &&
        (memcmp(src_data, MBCRYPTTAG_V2_STR, MBCRYPTTAG_LEN) == 0 ||
         memcmp(src_data, MBCRYPTTAG_V3_STR, MBCRYPTTAG_LEN) == 0)) {
        free(src_data);
        php_error_docref(NULL, E_WARNING,
            "mbcryptload_encrypt_file: '%s' is already encrypted", src);
        RETURN_FALSE;
    }

    enc_data = mbcrypt_lib_encode(src_data, src_sz, &enc_sz,
                                   mode, (pass_len > 0) ? pass : NULL);
    free(src_data);

    if (!enc_data || !enc_sz) {
        php_error_docref(NULL, E_WARNING,
            "mbcryptload_encrypt_file: encryption failed");
        RETURN_FALSE;
    }

    fp = fopen(dst, "wb");
    if (!fp) {
        free(enc_data);
        php_error_docref(NULL, E_WARNING,
            "mbcryptload_encrypt_file: cannot create '%s'", dst);
        RETURN_FALSE;
    }
    if (fwrite(enc_data, 1, enc_sz, fp) != enc_sz) {
        fclose(fp);
        free(enc_data);
        php_error_docref(NULL, E_WARNING,
            "mbcryptload_encrypt_file: write error on '%s'", dst);
        RETURN_FALSE;
    }
    fclose(fp);
    free(enc_data);
    RETURN_TRUE;
}

/* string mbcryptload_version()
 * Return the extension version string.                               */
PHP_FUNCTION(mbcryptload_version)
{
    ZEND_PARSE_PARAMETERS_NONE();
    RETURN_STRING(PHP_MBCRYPTLOAD_VERSION);
}

/* ==================================================================
 * .mbpak archive support (see ../mbpak/FORMAT.md for the format and
 * ../mbpak/mbpak_lib.h for the C API these functions bind to).
 *
 * A single archive can be mounted at a time (mirrors the single
 * runtime `password` above). The mount persists across requests in
 * the same worker -- same lifecycle as `password` -- and is closed
 * from MSHUTDOWN or an explicit mbpak_unmount()/mbpak_mount() replace.
 * ================================================================== */

/* bool mbpak_mount(string $archive, string $password)
 * Open a .mbpak archive and decrypt its TOC. A wrong password fails
 * here immediately (GCM authentication on the TOC record). Replaces
 * any previously mounted archive.                                    */
PHP_FUNCTION(mbpak_mount)
{
    char *archive, *pass;
    size_t archive_len, pass_len;
    mbpak_t *pak;

    ZEND_PARSE_PARAMETERS_START(2, 2)
        Z_PARAM_STRING(archive, archive_len)
        Z_PARAM_STRING(pass, pass_len)
    ZEND_PARSE_PARAMETERS_END();

    pak = mbpak_open(archive, pass);
    if (!pak) {
        php_error_docref(NULL, E_WARNING,
            "mbpak_mount: cannot open '%s' (wrong password or corrupted archive)",
            archive);
        RETURN_FALSE;
    }

    if (MBCRYPTLOAD_G(mounted)) {
        mbpak_close(MBCRYPTLOAD_G(mounted));
    }
    MBCRYPTLOAD_G(mounted) = pak;

    /* PHP memoizes the result of the last stat() per filename (see
     * ext/standard/filestat.c) regardless of which stream wrapper
     * served it. Without this, a file_exists("mbpak://x") called
     * *before* this mount (correctly false, nothing mounted yet) would
     * keep returning the stale cached "false" even after mounting an
     * archive that does contain x -- exactly the bug this call exists
     * to prevent. Mirrors what php_plain_files_unlink()/rename() do
     * after any filesystem-visible change. */
    php_clear_stat_cache(0, NULL, 0);
    RETURN_TRUE;
}

/* bool mbpak_unmount()
 * Flush and close the currently mounted archive, if any.              */
PHP_FUNCTION(mbpak_unmount)
{
    ZEND_PARSE_PARAMETERS_NONE();

    if (!MBCRYPTLOAD_G(mounted)) RETURN_FALSE;

    mbpak_close(MBCRYPTLOAD_G(mounted));
    MBCRYPTLOAD_G(mounted) = NULL;

    /* Same reasoning as in mbpak_mount() above, symmetric case: a
     * previously-cached "exists" must not survive the unmount. */
    php_clear_stat_cache(0, NULL, 0);
    RETURN_TRUE;
}

/* array mbpak_list()
 * List the live (non-deleted) paths in the mounted archive. Empty
 * array if nothing is mounted.                                        */
PHP_FUNCTION(mbpak_list)
{
    const mbpak_entry_t *e;

    ZEND_PARSE_PARAMETERS_NONE();

    array_init(return_value);
    if (!MBCRYPTLOAD_G(mounted)) return;

    for (e = mbpak_list(MBCRYPTLOAD_G(mounted)); e; e = e->next) {
        if (e->flags & MBPAK_FLAG_TOMBSTONE) continue;
        add_next_index_string(return_value, e->path);
    }
}

/* string|false mbpak_get(string $vpath)
 * Read and decrypt one file from the mounted archive. This is what
 * the mbpak:// stream wrapper uses internally; call it directly when
 * you just need the bytes (no require/include involved).             */
PHP_FUNCTION(mbpak_get)
{
    char *vpath;
    size_t vpath_len;
    char *data;
    size_t len;

    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_STRING(vpath, vpath_len)
    ZEND_PARSE_PARAMETERS_END();

    if (!MBCRYPTLOAD_G(mounted)) {
        php_error_docref(NULL, E_WARNING,
            "mbpak_get: no archive mounted (call mbpak_mount() first)");
        RETURN_FALSE;
    }

    data = mbpak_get(MBCRYPTLOAD_G(mounted), vpath, &len);
    if (!data) RETURN_FALSE;

    RETVAL_STRINGL(data, len);
    free(data);
}

/* bool mbpak_put(string $vpath, string $data)
 * Insert or replace one file in the mounted archive. Appends the new
 * record and commits (flushes) immediately -- each call is a durable
 * write, matching the archive's real append + commit protocol.        */
PHP_FUNCTION(mbpak_put)
{
    char *vpath, *data;
    size_t vpath_len, data_len;

    ZEND_PARSE_PARAMETERS_START(2, 2)
        Z_PARAM_STRING(vpath, vpath_len)
        Z_PARAM_STRING(data, data_len)
    ZEND_PARSE_PARAMETERS_END();

    if (!MBCRYPTLOAD_G(mounted)) {
        php_error_docref(NULL, E_WARNING,
            "mbpak_put: no archive mounted (call mbpak_mount() first)");
        RETURN_FALSE;
    }

    if (mbpak_put(MBCRYPTLOAD_G(mounted), vpath, data, data_len) != 0 ||
        mbpak_flush(MBCRYPTLOAD_G(mounted)) != 0) {
        php_error_docref(NULL, E_WARNING, "mbpak_put: write failed");
        RETURN_FALSE;
    }
    /* A previously-stat'd size/mtime (or a cached "doesn't exist") for
     * this vpath must not survive a write -- see mbpak_mount() above. */
    php_clear_stat_cache(0, NULL, 0);
    RETURN_TRUE;
}

/* bool mbpak_delete(string $vpath)
 * Tombstone one file (space reclaimed by mbpak_compact()). Commits
 * immediately.                                                        */
PHP_FUNCTION(mbpak_delete)
{
    char *vpath;
    size_t vpath_len;

    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_STRING(vpath, vpath_len)
    ZEND_PARSE_PARAMETERS_END();

    if (!MBCRYPTLOAD_G(mounted)) {
        php_error_docref(NULL, E_WARNING,
            "mbpak_delete: no archive mounted (call mbpak_mount() first)");
        RETURN_FALSE;
    }

    if (mbpak_delete(MBCRYPTLOAD_G(mounted), vpath) != 0 ||
        mbpak_flush(MBCRYPTLOAD_G(mounted)) != 0) {
        RETURN_FALSE;
    }
    /* Same reasoning as mbpak_put() above: a cached "exists" for this
     * vpath must not survive the delete. */
    php_clear_stat_cache(0, NULL, 0);
    RETURN_TRUE;
}

/* bool mbpak_compact()
 * Rewrite the mounted archive keeping only live records. On success
 * the same mount stays usable (reopened transparently).               */
PHP_FUNCTION(mbpak_compact)
{
    ZEND_PARSE_PARAMETERS_NONE();

    if (!MBCRYPTLOAD_G(mounted)) {
        php_error_docref(NULL, E_WARNING,
            "mbpak_compact: no archive mounted (call mbpak_mount() first)");
        RETURN_FALSE;
    }

    if (mbpak_compact(MBCRYPTLOAD_G(mounted)) != 0) {
        php_error_docref(NULL, E_WARNING,
            "mbpak_compact: failed (archive left untouched)");
        RETURN_FALSE;
    }
    RETURN_TRUE;
}

/* ==================================================================
 * mbpak:// read-only stream wrapper
 *
 * Serves a file out of the mounted archive as an in-memory PHP stream,
 * so that `require "mbpak://path"`, `include`, `file_get_contents()`,
 * `fopen(..., "r")` etc. work transparently. Each open is a single
 * mbpak_get() call -- decryption already happened there, there is no
 * separate crypto step for this wrapper.
 *
 * Deliberately read-only: fopen(..., "w") on this scheme is refused.
 * Writing through a POSIX-shaped stream would have to fake semantics
 * the archive doesn't have (in-place byte ranges); mbpak_put() /
 * mbpak_delete() / mbpak_compact() above already expose the archive's
 * actual append + commit protocol directly, which is more honest than
 * pretending it's a normal file.
 *
 * url_stat is implemented (see mbpak_stream_url_stat below) so
 * file_exists()/is_file()/filesize()/filemtime() work without opening
 * anything -- required for Composer's autoloader (and anything else
 * that probes candidate paths) to function against mbpak:// paths.
 * stream_stat (fstat() on an already-open handle) is not implemented;
 * nothing in this extension needs it yet.
 * ================================================================== */

static php_stream *mbpak_stream_opener(php_stream_wrapper *wrapper,
                                        const char *path, const char *mode,
                                        int options, zend_string **opened_path,
                                        php_stream_context *context STREAMS_DC)
{
    const char *vpath;
    char *data;
    size_t len;
    php_stream *stream;

    (void)wrapper;
    (void)context;

    if (opened_path) *opened_path = NULL;

    if (mode[0] != 'r') {
        if (options & REPORT_ERRORS) {
            php_error_docref(NULL, E_WARNING,
                "mbpak wrapper is read-only; use mbpak_put() to write into a mounted archive");
        }
        return NULL;
    }

    if (strncmp(path, "mbpak://", 8) != 0) return NULL;
    vpath = path + 8;

    if (!MBCRYPTLOAD_G(mounted)) {
        if (options & REPORT_ERRORS) {
            php_error_docref(NULL, E_WARNING,
                "mbpak stream: no archive mounted (call mbpak_mount() first)");
        }
        return NULL;
    }

    data = mbpak_get(MBCRYPTLOAD_G(mounted), vpath, &len);
    if (!data) {
        if (options & REPORT_ERRORS) {
            php_error_docref(NULL, E_WARNING,
                "mbpak stream: '%s' not found in the mounted archive, or authentication failed",
                vpath);
        }
        return NULL;
    }

#if PHP_VERSION_ID >= 80100
    /* PHP 8.1+: php_stream_memory_open() takes a zend_string* and
     * addrefs it into the stream (see main/streams/memory.c:
     * ms->data = zend_string_copy(buf)) rather than taking ownership
     * of it outright -- release our own reference right after so the
     * stream ends up holding the only one, freed automatically when
     * it is closed. */
    {
        zend_string *zbuf = zend_string_init(data, len, 0);
        free(data);
        stream = php_stream_memory_open(TEMP_STREAM_READONLY, zbuf);
        zend_string_release(zbuf);
    }
#else
    /* PHP < 8.1: php_stream_memory_open() took a raw (mode, buf, length)
     * triple instead of a zend_string*. With TEMP_STREAM_TAKE_BUFFER it
     * takes ownership of an emalloc()'d buffer outright and efree()s it
     * itself when the stream closes (see php_stream_memory_close()) --
     * TEMP_STREAM_READONLY would instead never free it at all on this
     * PHP version, leaking until end-of-request. */
    {
        char *ebuf = emalloc(len ? len : 1);
        memcpy(ebuf, data, len);
        free(data);
        stream = php_stream_memory_open(TEMP_STREAM_TAKE_BUFFER, ebuf, len);
    }
#endif

    return stream;
}

/* int url_stat("mbpak://path", flags, ssb)
 *
 * Answers file_exists()/is_file()/filesize()/filemtime() without
 * decrypting anything: everything needed (size, mtime) is already sat
 * in the TOC in plaintext (see mbpak_lib.h's format doc for why `size`
 * is duplicated there on purpose). This matters beyond purity -- PHP's
 * Composer autoloader calls file_exists() on every PSR-4 candidate
 * path while resolving a class, so a full AES-GCM decrypt + zlib
 * inflate per stat() would be a real per-request cost, not just one.
 *
 * Modelled directly on ext/phar/stream.c's phar_wrapper_stat(), which
 * faces the same problem (synthesizing a stat for an archive member
 * with no real inode behind it).                                     */
static int mbpak_stream_url_stat(php_stream_wrapper *wrapper, const char *url,
                                  int flags, php_stream_statbuf *ssb,
                                  php_stream_context *context)
{
    const mbpak_entry_t *e;
    const char *vpath;

    (void)wrapper;
    (void)context;

    if (strncmp(url, "mbpak://", 8) != 0) return -1;
    vpath = url + 8;

    if (!MBCRYPTLOAD_G(mounted)) {
        if (!(flags & PHP_STREAM_URL_STAT_QUIET)) {
            php_error_docref(NULL, E_WARNING,
                "mbpak stat: no archive mounted (call mbpak_mount() first)");
        }
        return -1;
    }

    for (e = mbpak_list(MBCRYPTLOAD_G(mounted)); e; e = e->next) {
        if ((e->flags & MBPAK_FLAG_TOMBSTONE) == 0 && strcmp(e->path, vpath) == 0) {
            break;
        }
    }
    if (!e) return -1; /* not found -- deliberately silent even if not
                         * quiet: this is the expected outcome of most
                         * file_exists() calls an autoloader makes    */

    memset(ssb, 0, sizeof(php_stream_statbuf));
    ssb->sb.st_mode  = S_IFREG | 0444; /* regular file, read-only     */
    ssb->sb.st_nlink = 1;
    ssb->sb.st_size  = (off_t)   e->size;
    ssb->sb.st_mtime = (time_t)  e->mtime;
    ssb->sb.st_atime = (time_t)  e->mtime;
    ssb->sb.st_ctime = (time_t)  e->mtime;

    return 0;
}

static const php_stream_wrapper_ops mbpak_stream_wrapper_ops = {
    mbpak_stream_opener,
    NULL, /* stream_closer -- default stream teardown is enough        */
    NULL, /* stream_stat   -- no metadata beyond content                */
    mbpak_stream_url_stat,
    NULL, /* dir_opener    -- no directory listing through the wrapper  */
    "mbpak",
    NULL, /* unlink        -- use mbpak_delete() instead                */
    NULL, /* rename                                                     */
    NULL, /* stream_mkdir                                               */
    NULL, /* stream_rmdir                                               */
    NULL  /* stream_metadata                                            */
};

static php_stream_wrapper mbpak_stream_wrapper = {
    &mbpak_stream_wrapper_ops,
    NULL,
    0
};

/* ==================================================================
 * Function table
 * ================================================================== */
static const zend_function_entry mbcryptload_functions[] = {
    PHP_FE(mbcryptload_set_password, arginfo_mbcryptload_set_password)
    PHP_FE(mbcryptload_encrypt_file, arginfo_mbcryptload_encrypt_file)
    PHP_FE(mbcryptload_version,      arginfo_mbcryptload_version)
    PHP_FE(mbpak_mount,              arginfo_mbpak_mount)
    PHP_FE(mbpak_unmount,            arginfo_mbpak_unmount)
    PHP_FE(mbpak_list,               arginfo_mbpak_list)
    PHP_FE(mbpak_get,                arginfo_mbpak_get)
    PHP_FE(mbpak_put,                arginfo_mbpak_put)
    PHP_FE(mbpak_delete,             arginfo_mbpak_delete)
    PHP_FE(mbpak_compact,            arginfo_mbpak_compact)
    PHP_FE_END
};

/* ==================================================================
 * Module lifecycle
 * ================================================================== */
PHP_MINIT_FUNCTION(mbcryptload)
{
    ZEND_INIT_MODULE_GLOBALS(mbcryptload,
        mbcryptload_globals_ctor,
        NULL);

    REGISTER_INI_ENTRIES();

    /* Hook into the Zend compiler */
    org_compile_file  = zend_compile_file;
    zend_compile_file = mbcryptload_compile_file;

    /* Register the mbpak:// read-only stream wrapper (see the wrapper's
     * doc comment above). Purely additive: unrelated to, and never
     * consulted by, the compile_file hook above for ordinary disk
     * paths -- fh_read_file() there uses plain fopen(), which simply
     * fails (and falls back to org_compile_file) for a "mbpak://..."
     * string, letting the original compiler route it through this
     * wrapper instead. */
    if (php_register_url_stream_wrapper("mbpak", &mbpak_stream_wrapper) != SUCCESS) {
        return FAILURE;
    }

    return SUCCESS;
}

PHP_MSHUTDOWN_FUNCTION(mbcryptload)
{
    /* Restore original compiler hook */
    zend_compile_file = org_compile_file;

    php_unregister_url_stream_wrapper("mbpak");

    UNREGISTER_INI_ENTRIES();

    /* See comment on mbcryptload_globals_ctor above: cleanup is done here
     * rather than via a globals_dtor registered with TSRM. password is
     * persistently allocated (see mbcryptload_set_password), so pefree()
     * is required here instead of efree(). */
    if (MBCRYPTLOAD_G(password)) {
        pefree(MBCRYPTLOAD_G(password), 1);
        MBCRYPTLOAD_G(password) = NULL;
    }

    /* mbpak_t is plain malloc/free (mbpak_lib has no PHP dependency, see
     * ../mbpak/mbpak_lib.h) -- safe to free unconditionally here, same
     * reasoning as for `password` above. */
    if (MBCRYPTLOAD_G(mounted)) {
        mbpak_close(MBCRYPTLOAD_G(mounted));
        MBCRYPTLOAD_G(mounted) = NULL;
    }

    return SUCCESS;
}

PHP_RINIT_FUNCTION(mbcryptload)
{
#if defined(ZTS) && defined(COMPILE_DL_MBCRYPTLOAD)
    ZEND_TSRMLS_CACHE_UPDATE();
#endif
    return SUCCESS;
}

PHP_RSHUTDOWN_FUNCTION(mbcryptload)
{
    return SUCCESS;
}

PHP_MINFO_FUNCTION(mbcryptload)
{
    php_info_print_table_start();
    php_info_print_table_header(2, "mbcryptload", "enabled");
    php_info_print_table_row(2, "Version", PHP_MBCRYPTLOAD_VERSION);
    php_info_print_table_row(2, "Author",
        "CISSE Mohamed B <mohamedbachir.cisse@gmail.com>");
#ifdef MBCRYPT_HAS_OPENSSL
    php_info_print_table_row(2, "AES-256-GCM",    "enabled");
#else
    php_info_print_table_row(2, "AES-256-GCM",    "disabled (no OpenSSL)");
#endif
    php_info_print_table_row(2, "V2 compatibility","enabled");
    php_info_print_table_row(2, "mbpak:// wrapper", "enabled");
    php_info_print_table_row(2, "mbpak archive mounted",
        MBCRYPTLOAD_G(mounted) ? "yes" : "no");
    php_info_print_table_row(2, "Thread safety",
#ifdef ZTS
        "enabled (ZTS)"
#else
        "disabled (NTS)"
#endif
    );
    {
        const char *ini_pass = MBCRYPTLOAD_G(ini_password);
        php_info_print_table_row(2, "mbcryptload.password",
            (ini_pass && *ini_pass) ? "set (hidden)" : "not set");
    }
    php_info_print_table_end();
}

/* ==================================================================
 * Module entry
 * ================================================================== */
zend_module_entry mbcryptload_module_entry = {
    STANDARD_MODULE_HEADER,
    "mbcryptload",
    mbcryptload_functions,
    PHP_MINIT(mbcryptload),
    PHP_MSHUTDOWN(mbcryptload),
    PHP_RINIT(mbcryptload),
    PHP_RSHUTDOWN(mbcryptload),
    PHP_MINFO(mbcryptload),
    PHP_MBCRYPTLOAD_VERSION,
    STANDARD_MODULE_PROPERTIES
};

#ifdef COMPILE_DL_MBCRYPTLOAD
#  ifdef ZTS
ZEND_TSRMLS_CACHE_DEFINE()
#  endif
ZEND_GET_MODULE(mbcryptload)
#endif
