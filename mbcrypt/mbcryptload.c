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
#include "php_mbcryptload.h"
#include "mbcrypt_lib.h"

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
    char *password;     /* runtime password set via mbcryptload_set_password() */
    char *ini_password; /* password set via mbcryptload.password in php.ini    */
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
}

static void mbcryptload_globals_dtor(zend_mbcryptload_globals *g)
{
    if (g->password) {
        efree(g->password);
        g->password = NULL;
    }
}

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

    if (MBCRYPTLOAD_G(password)) {
        efree(MBCRYPTLOAD_G(password));
    }
    MBCRYPTLOAD_G(password) = estrndup(pass, pass_len);
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
 * Function table
 * ================================================================== */
static const zend_function_entry mbcryptload_functions[] = {
    PHP_FE(mbcryptload_set_password, arginfo_mbcryptload_set_password)
    PHP_FE(mbcryptload_encrypt_file, arginfo_mbcryptload_encrypt_file)
    PHP_FE(mbcryptload_version,      arginfo_mbcryptload_version)
    PHP_FE_END
};

/* ==================================================================
 * Module lifecycle
 * ================================================================== */
PHP_MINIT_FUNCTION(mbcryptload)
{
    ZEND_INIT_MODULE_GLOBALS(mbcryptload,
        mbcryptload_globals_ctor,
        mbcryptload_globals_dtor);

    REGISTER_INI_ENTRIES();

    /* Hook into the Zend compiler */
    org_compile_file  = zend_compile_file;
    zend_compile_file = mbcryptload_compile_file;

    return SUCCESS;
}

PHP_MSHUTDOWN_FUNCTION(mbcryptload)
{
    /* Restore original compiler hook */
    zend_compile_file = org_compile_file;

    UNREGISTER_INI_ENTRIES();

#ifndef ZTS
    /* In non-ZTS builds globals_dtor is not called automatically */
    mbcryptload_globals_dtor(&mbcryptload_globals);
#endif

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
