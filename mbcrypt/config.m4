dnl ---------------------------------------------------------------------------
dnl mbcryptload PHP extension — autoconf configuration
dnl
dnl Usage:
dnl   phpize && ./configure --enable-mbcryptload
dnl   phpize && ./configure --enable-mbcryptload --with-openssl-dir=/usr
dnl ---------------------------------------------------------------------------

PHP_ARG_ENABLE([mbcryptload],
  [whether to enable mbcryptload support],
  [AS_HELP_STRING([--enable-mbcryptload],
    [Enable mbcryptload transparent PHP encryption])],
  [no])

PHP_ARG_WITH([openssl-dir],
  [for OpenSSL installation prefix (enables AES-256-GCM)],
  [AS_HELP_STRING([--with-openssl-dir=DIR],
    [Path to OpenSSL install prefix (auto-detected if omitted)])],
  [auto],
  [no])

if test "$PHP_MBCRYPTLOAD" != "no"; then

  dnl -----------------------------------------------------------------------
  dnl zlib (required)
  dnl -----------------------------------------------------------------------
  AC_CHECK_HEADER([zlib.h], [],
    [AC_MSG_ERROR([zlib.h not found. Install zlib-dev / zlib-devel.])])

  PHP_CHECK_LIBRARY(z, deflate,
    [PHP_ADD_LIBRARY(z, 1, MBCRYPTLOAD_SHARED_LIBADD)],
    [AC_MSG_ERROR([zlib library (libz) not found.])],
    [-lz])

  dnl -----------------------------------------------------------------------
  dnl OpenSSL (optional — enables AES-256-GCM)
  dnl -----------------------------------------------------------------------
  AC_MSG_CHECKING([for OpenSSL (AES-256-GCM support)])

  OPENSSL_FOUND=no

  dnl Try pkg-config first (most reliable on modern systems)
  if test "$PHP_OPENSSL_DIR" = "auto"; then
    if pkg-config --exists openssl 2>/dev/null; then
      OPENSSL_INCS=$(pkg-config --cflags openssl)
      OPENSSL_LIBS=$(pkg-config --libs   openssl)
      PHP_EVAL_INCLINE($OPENSSL_INCS)
      PHP_EVAL_LIBLINE($OPENSSL_LIBS, MBCRYPTLOAD_SHARED_LIBADD)
      AC_DEFINE([MBCRYPT_HAS_OPENSSL], [1], [OpenSSL available for AES-256-GCM])
      OPENSSL_FOUND=yes
    else
      dnl Fall back to common installation paths
      for d in /usr /usr/local /opt/homebrew /opt/local; do
        if test -r "$d/include/openssl/evp.h"; then
          PHP_OPENSSL_DIR="$d"
          break
        fi
      done
    fi
  fi

  if test "$OPENSSL_FOUND" = "no" && test "$PHP_OPENSSL_DIR" != "auto"; then
    if test -r "$PHP_OPENSSL_DIR/include/openssl/evp.h"; then
      PHP_ADD_INCLUDE($PHP_OPENSSL_DIR/include)
      PHP_ADD_LIBRARY_WITH_PATH(crypto, $PHP_OPENSSL_DIR/$PHP_LIBDIR,
        MBCRYPTLOAD_SHARED_LIBADD)
      PHP_ADD_LIBRARY_WITH_PATH(ssl,    $PHP_OPENSSL_DIR/$PHP_LIBDIR,
        MBCRYPTLOAD_SHARED_LIBADD)
      AC_DEFINE([MBCRYPT_HAS_OPENSSL], [1], [OpenSSL available for AES-256-GCM])
      OPENSSL_FOUND=yes
    fi
  fi

  if test "$OPENSSL_FOUND" = "yes"; then
    AC_MSG_RESULT([yes — AES-256-GCM enabled])
  else
    AC_MSG_RESULT([no — AES-256-GCM disabled (legacy XOR only)])
  fi

  dnl -----------------------------------------------------------------------
  dnl Build the extension
  dnl -----------------------------------------------------------------------
  PHP_NEW_EXTENSION(mbcryptload,
    mbcryptload.c mbcrypt_lib.c,
    $ext_shared)

  PHP_SUBST(MBCRYPTLOAD_SHARED_LIBADD)
fi
