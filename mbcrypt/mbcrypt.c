/*
   +----------------------------------------------------------------------+
   | mbcrypt - PHP file encryption CLI tool                               |
   +----------------------------------------------------------------------+
   | Author  : CISSE Mohamed B <mohamedbachir.cisse@gmail.com>           |
   | Original: sun bing <hoowa.sun@gmail.com>                            |
   +----------------------------------------------------------------------+
   | Usage (single file):
   |   mbcrypt [-p password] -o output.php source.php
   | Usage (recursive directory):
   |   mbcrypt [-p password] -r -o output_dir/ source_dir/
   +----------------------------------------------------------------------+
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mbcrypt_lib.h"

/* ---- Platform detection ------------------------------------------ */
#ifdef _WIN32
#  include <windows.h>
#  define PATH_SEP '\\'
   typedef int mbool;
#else
#  include <dirent.h>
#  include <sys/stat.h>
#  include <sys/types.h>
#  include <unistd.h>
#  define PATH_SEP '/'
   typedef int mbool;
#endif

#define VERSION "3.0.0"

/* ================================================================== */
/* I/O helpers                                                         */
/* ================================================================== */

/* Read entire file into a malloc buffer. Caller must free().         */
static char *read_file(const char *path, size_t *out_len)
{
    FILE *fp;
    long fsize;
    char *buf;

    *out_len = 0;
    fp = fopen(path, "rb");
    if (!fp) return NULL;

    if (fseek(fp, 0, SEEK_END) != 0 || (fsize = ftell(fp)) < 0) {
        fclose(fp);
        return NULL;
    }
    rewind(fp);

    *out_len = (size_t)fsize;
    buf = (char *)malloc(*out_len + 1);
    if (!buf) { fclose(fp); *out_len = 0; return NULL; }
    buf[*out_len] = '\0';

    if (*out_len > 0 && fread(buf, 1, *out_len, fp) != *out_len) {
        fclose(fp);
        free(buf);
        *out_len = 0;
        return NULL;
    }
    fclose(fp);
    return buf;
}

/* Write buffer to file. Returns 0 on success, -1 on error.          */
static int write_file(const char *path, const char *data, size_t len)
{
    FILE *fp = fopen(path, "wb");
    if (!fp) return -1;
    if (fwrite(data, 1, len, fp) != len) { fclose(fp); return -1; }
    fclose(fp);
    return 0;
}

/* ================================================================== */
/* Single-file encryption                                              */
/* ================================================================== */

/* Returns:  0 = ok, 1 = skipped (already encrypted), -1 = error     */
static int encrypt_file(const char *src, const char *dst,
                         int mode, const char *password)
{
    char *src_data, *enc_data;
    size_t src_len = 0, enc_len = 0;

    src_data = read_file(src, &src_len);
    if (!src_data) {
        fprintf(stderr, "Error: cannot read '%s'\n", src);
        return -1;
    }

    /* Refuse to double-encrypt */
    if (src_len >= MBCRYPTTAG_LEN &&
        (memcmp(src_data, MBCRYPTTAG_V2_STR, MBCRYPTTAG_LEN) == 0 ||
         memcmp(src_data, MBCRYPTTAG_V3_STR, MBCRYPTTAG_LEN) == 0)) {
        fprintf(stderr, "Skip (already encrypted): %s\n", src);
        free(src_data);
        return 1;
    }

    enc_data = mbcrypt_lib_encode(src_data, src_len, &enc_len, mode, password);
    free(src_data);

    if (!enc_data || !enc_len) {
        fprintf(stderr, "Error: encryption failed for '%s'\n", src);
        return -1;
    }

    if (write_file(dst, enc_data, enc_len) != 0) {
        fprintf(stderr, "Error: cannot write '%s'\n", dst);
        free(enc_data);
        return -1;
    }

    free(enc_data);
    printf("OK: %s -> %s\n", src, dst);
    return 0;
}

/* ================================================================== */
/* Path utilities                                                      */
/* ================================================================== */

static void join_path(char *out, size_t sz,
                       const char *dir, const char *name)
{
    size_t dlen = strlen(dir);
    if (dlen && dir[dlen - 1] != '/' && dir[dlen - 1] != '\\')
        snprintf(out, sz, "%s%c%s", dir, PATH_SEP, name);
    else
        snprintf(out, sz, "%s%s", dir, name);
}

static mbool is_php(const char *name)
{
    size_t n = strlen(name);
    return (n > 4 && strcmp(name + n - 4, ".php") == 0);
}

/* Create directory hierarchy (no-op if it already exists).           */
static void mkdirs(const char *path)
{
    char buf[4096];
    size_t i, n;

    strncpy(buf, path, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    n = strlen(buf);

    for (i = 1; i <= n; i++) {
        char c = buf[i];
        if (i == n || buf[i] == '/' || buf[i] == '\\') {
            buf[i] = '\0';
#ifdef _WIN32
            CreateDirectoryA(buf, NULL);
#else
            mkdir(buf, 0755);
#endif
            buf[i] = c;
        }
    }
}

/* ================================================================== */
/* Recursive directory encryption                                      */
/* ================================================================== */

static void encrypt_dir(const char *src_dir, const char *dst_dir,
                         int mode, const char *password,
                         int *n_ok, int *n_skip, int *n_err)
{
    char src_path[4096], dst_path[4096];

    mkdirs(dst_dir);

#ifdef _WIN32
    /* --- Windows: FindFirstFile / FindNextFile --- */
    char pattern[4096];
    WIN32_FIND_DATAA fd;
    HANDLE h;

    snprintf(pattern, sizeof(pattern), "%s\\*", src_dir);
    h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "Warning: cannot open directory '%s'\n", src_dir);
        return;
    }
    do {
        if (strcmp(fd.cFileName, ".") == 0 ||
            strcmp(fd.cFileName, "..") == 0) continue;

        join_path(src_path, sizeof(src_path), src_dir, fd.cFileName);
        join_path(dst_path, sizeof(dst_path), dst_dir, fd.cFileName);

        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            encrypt_dir(src_path, dst_path, mode, password, n_ok, n_skip, n_err);
        } else if (is_php(fd.cFileName)) {
            int r = encrypt_file(src_path, dst_path, mode, password);
            if      (r == 0)  (*n_ok)++;
            else if (r == 1)  (*n_skip)++;
            else              (*n_err)++;
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);

#else
    /* --- POSIX: opendir / readdir --- */
    DIR *d = opendir(src_dir);
    struct dirent *ent;
    struct stat st;

    if (!d) {
        fprintf(stderr, "Warning: cannot open directory '%s'\n", src_dir);
        return;
    }
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 ||
            strcmp(ent->d_name, "..") == 0) continue;

        join_path(src_path, sizeof(src_path), src_dir, ent->d_name);
        join_path(dst_path, sizeof(dst_path), dst_dir, ent->d_name);

        if (stat(src_path, &st) != 0) continue;

        if (S_ISDIR(st.st_mode)) {
            encrypt_dir(src_path, dst_path, mode, password, n_ok, n_skip, n_err);
        } else if (S_ISREG(st.st_mode) && is_php(ent->d_name)) {
            int r = encrypt_file(src_path, dst_path, mode, password);
            if      (r == 0)  (*n_ok)++;
            else if (r == 1)  (*n_skip)++;
            else              (*n_err)++;
        }
    }
    closedir(d);
#endif
}

/* ================================================================== */
/* CLI                                                                 */
/* ================================================================== */

static void print_usage(const char *prog)
{
    printf("Usage: %s [OPTIONS] -o OUTPUT SOURCE\n", prog);
    printf("       %s [OPTIONS] -r -o OUTPUT_DIR SOURCE_DIR\n\n", prog);
    printf("Options:\n");
    printf("  -o, --output FILE    Output file or directory\n");
    printf("  -r, --recursive      Encrypt all .php files recursively\n");
    printf("  -p, --password PASS  Use AES-256-GCM encryption\n");
    printf("      --legacy         Force legacy XOR mode\n");
    printf("  -h, --help           Show this help message\n");
    printf("  -v, --version        Show version information\n");
}

static void print_version(void)
{
    printf("mbcrypt %s\n", VERSION);
    printf("Author: CISSE Mohamed B <mohamedbachir.cisse@gmail.com>\n");
#ifdef MBCRYPT_HAS_OPENSSL
    printf("AES-256-GCM: enabled (OpenSSL)\n");
#else
    printf("AES-256-GCM: disabled (rebuild with OpenSSL)\n");
#endif
}

int main(int argc, char **argv)
{
    const char *output   = NULL;
    const char *password = NULL;
    const char *source   = NULL;
    int mode      = MBCRYPT_LEGACY_MODE;
    int recursive = 0;
    int i;

    for (i = 1; i < argc; i++) {
        const char *a = argv[i];

        if ((strcmp(a, "-o") == 0 || strcmp(a, "--output") == 0) &&
            i + 1 < argc) {
            output = argv[++i];

        } else if ((strcmp(a, "-p") == 0 || strcmp(a, "--password") == 0) &&
                   i + 1 < argc) {
            password = argv[++i];
            mode     = MBCRYPT_AES_GCM_MODE;

        } else if (strcmp(a, "-r") == 0 || strcmp(a, "--recursive") == 0) {
            recursive = 1;

        } else if (strcmp(a, "--legacy") == 0) {
            mode     = MBCRYPT_LEGACY_MODE;
            password = NULL;

        } else if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
            print_usage(argv[0]);
            return 0;

        } else if (strcmp(a, "-v") == 0 || strcmp(a, "--version") == 0) {
            print_version();
            return 0;

        } else if (a[0] != '-') {
            source = a;

        } else {
            fprintf(stderr, "Unknown option: %s\n\n", a);
            print_usage(argv[0]);
            return 1;
        }
    }

    if (!source || !output) {
        fprintf(stderr, "Error: source and -o OUTPUT are required.\n\n");
        print_usage(argv[0]);
        return 1;
    }

#ifndef MBCRYPT_HAS_OPENSSL
    if (mode == MBCRYPT_AES_GCM_MODE) {
        fprintf(stderr,
            "Warning: OpenSSL not available in this build — "
            "falling back to legacy XOR mode.\n");
        mode = MBCRYPT_LEGACY_MODE;
    }
#endif

    if (recursive) {
        int n_ok = 0, n_skip = 0, n_err = 0;
        encrypt_dir(source, output, mode, password,
                    &n_ok, &n_skip, &n_err);
        printf("\nDone: %d encrypted, %d skipped, %d errors\n",
               n_ok, n_skip, n_err);
        return n_err ? 1 : 0;
    }

    return encrypt_file(source, output, mode, password) < 0 ? 1 : 0;
}
