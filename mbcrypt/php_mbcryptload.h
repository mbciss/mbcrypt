/*
   +----------------------------------------------------------------------+
   | mbcryptload PHP extension header                                     |
   +----------------------------------------------------------------------+
*/
#ifndef PHP_MBCRYPTLOAD_H
#define PHP_MBCRYPTLOAD_H

extern zend_module_entry mbcryptload_module_entry;
#define phpext_mbcryptload_ptr &mbcryptload_module_entry

#define PHP_MBCRYPTLOAD_VERSION "3.0.0"

#if defined(ZTS) && defined(COMPILE_DL_MBCRYPTLOAD)
ZEND_TSRMLS_CACHE_EXTERN()
#endif

PHP_FUNCTION(mbcryptload_set_password);
PHP_FUNCTION(mbcryptload_encrypt_file);
PHP_FUNCTION(mbcryptload_version);

/* .mbpak archive mounting (see ../mbpak/FORMAT.md) -- read via the
 * mbpak:// stream wrapper (require/include/file_get_contents), write
 * via these explicit functions. */
PHP_FUNCTION(mbpak_mount);
PHP_FUNCTION(mbpak_unmount);
PHP_FUNCTION(mbpak_list);
PHP_FUNCTION(mbpak_get);
PHP_FUNCTION(mbpak_put);
PHP_FUNCTION(mbpak_delete);
PHP_FUNCTION(mbpak_compact);

/* ------------------------------------------------------------------
 * Argument info (required by PHP 8.x to suppress arginfo warnings)
 * ------------------------------------------------------------------ */

/* bool mbcryptload_set_password(string $password) */
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(
    arginfo_mbcryptload_set_password, 0, 1, _IS_BOOL, 0)
    ZEND_ARG_TYPE_INFO(0, password, IS_STRING, 0)
ZEND_END_ARG_INFO()

/* bool mbcryptload_encrypt_file(
 *     string $source, string $dest
 *     [, string $password [, bool $use_aes = false]] ) */
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(
    arginfo_mbcryptload_encrypt_file, 0, 2, _IS_BOOL, 0)
    ZEND_ARG_TYPE_INFO(0, source,  IS_STRING, 0)
    ZEND_ARG_TYPE_INFO(0, dest,    IS_STRING, 0)
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, password, IS_STRING, 1, "null")
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, use_aes,  _IS_BOOL,  0, "false")
ZEND_END_ARG_INFO()

/* string mbcryptload_version() */
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(
    arginfo_mbcryptload_version, 0, 0, IS_STRING, 0)
ZEND_END_ARG_INFO()

/* bool mbpak_mount(string $archive, string $password) */
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(
    arginfo_mbpak_mount, 0, 2, _IS_BOOL, 0)
    ZEND_ARG_TYPE_INFO(0, archive,  IS_STRING, 0)
    ZEND_ARG_TYPE_INFO(0, password, IS_STRING, 0)
ZEND_END_ARG_INFO()

/* bool mbpak_unmount() */
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(
    arginfo_mbpak_unmount, 0, 0, _IS_BOOL, 0)
ZEND_END_ARG_INFO()

/* array mbpak_list() */
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(
    arginfo_mbpak_list, 0, 0, IS_ARRAY, 0)
ZEND_END_ARG_INFO()

/* string|false mbpak_get(string $vpath) */
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_MASK_EX(
    arginfo_mbpak_get, 0, 1, MAY_BE_STRING|MAY_BE_FALSE)
    ZEND_ARG_TYPE_INFO(0, vpath, IS_STRING, 0)
ZEND_END_ARG_INFO()

/* bool mbpak_put(string $vpath, string $data) */
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(
    arginfo_mbpak_put, 0, 2, _IS_BOOL, 0)
    ZEND_ARG_TYPE_INFO(0, vpath, IS_STRING, 0)
    ZEND_ARG_TYPE_INFO(0, data,  IS_STRING, 0)
ZEND_END_ARG_INFO()

/* bool mbpak_delete(string $vpath) */
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(
    arginfo_mbpak_delete, 0, 1, _IS_BOOL, 0)
    ZEND_ARG_TYPE_INFO(0, vpath, IS_STRING, 0)
ZEND_END_ARG_INFO()

/* bool mbpak_compact() */
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(
    arginfo_mbpak_compact, 0, 0, _IS_BOOL, 0)
ZEND_END_ARG_INFO()

PHP_MINIT_FUNCTION(mbcryptload);
PHP_MSHUTDOWN_FUNCTION(mbcryptload);
PHP_RINIT_FUNCTION(mbcryptload);
PHP_RSHUTDOWN_FUNCTION(mbcryptload);
PHP_MINFO_FUNCTION(mbcryptload);

#endif /* PHP_MBCRYPTLOAD_H */
