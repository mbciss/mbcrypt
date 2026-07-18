/*
   +----------------------------------------------------------------------+
   | mbpak - CLI for the mbpak encrypted archive format                   |
   +----------------------------------------------------------------------+
   | Author  : CISSE Mohamed B <mohamedbachir.cisse@gmail.com>           |
   +----------------------------------------------------------------------+
   | Usage:
   |   mbpak pack    -p PASSWORD -o ARCHIVE.mbpak SOURCE_DIR
   |   mbpak list    -p PASSWORD ARCHIVE.mbpak
   |   mbpak extract -p PASSWORD -o OUT_DIR ARCHIVE.mbpak [VPATH]
   |   mbpak put     -p PASSWORD ARCHIVE.mbpak VPATH LOCAL_FILE
   |   mbpak get     -p PASSWORD ARCHIVE.mbpak VPATH [-o LOCAL_FILE]
   |   mbpak rm      -p PASSWORD ARCHIVE.mbpak VPATH
   |   mbpak compact -p PASSWORD ARCHIVE.mbpak
   +----------------------------------------------------------------------+
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mbpak_lib.h"

#ifdef _WIN32
#  include <windows.h>
#  define PATH_SEP '\\'
#else
#  include <dirent.h>
#  include <sys/stat.h>
#  include <sys/types.h>
#  include <unistd.h>
#  define PATH_SEP '/'
#endif

#define VERSION "1.0.0"

/* ================================================================== */
/* I/O helpers (same pattern as mbcrypt.c)                             */
/* ================================================================== */

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
    buf = (char *)malloc(*out_len ? *out_len : 1);
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

static int write_file(const char *path, const char *data, size_t len)
{
    FILE *fp = fopen(path, "wb");
    if (!fp) return -1;
    if (len && fwrite(data, 1, len, fp) != len) { fclose(fp); return -1; }
    fclose(fp);
    return 0;
}

/* Create directory hierarchy (no-op if it already exists). */
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

/* Normalize a filesystem-relative path into an archive vpath:
 * backslashes -> '/', strip any leading '/'. */
static void to_vpath(char *out, size_t sz, const char *in)
{
    size_t i = 0, j = 0;
    while (in[i] == '/' || in[i] == '\\') i++;
    for (; in[i] && j + 1 < sz; i++) {
        out[j++] = (in[i] == '\\') ? '/' : in[i];
    }
    out[j] = '\0';
}

/* ================================================================== */
/* pack: recursive directory walk -> mbpak_put() per regular file      */
/* ================================================================== */

static void pack_one(mbpak_t *pak, const char *full, const char *vpath,
                      int *n_ok, int *n_err)
{
    size_t len;
    char *data = read_file(full, &len);

    if (!data || !len || mbpak_put(pak, vpath, data, len) != 0) {
        fprintf(stderr, "Error: cannot pack '%s'\n", full);
        (*n_err)++;
    } else {
        printf("+ %s\n", vpath);
        (*n_ok)++;
    }
    free(data);
}

static void pack_dir(mbpak_t *pak, const char *dir, const char *base,
                      int *n_ok, int *n_err)
{
#ifdef _WIN32
    char pattern[4096], full[4096], rel[4096], vpath[4096];
    WIN32_FIND_DATAA fd;
    HANDLE h;

    snprintf(pattern, sizeof(pattern), "%s\\*", dir);
    h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "Warning: cannot open directory '%s'\n", dir);
        return;
    }
    do {
        if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0)
            continue;
        snprintf(full, sizeof(full), "%s\\%s", dir, fd.cFileName);

        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            pack_dir(pak, full, base, n_ok, n_err);
        } else {
            const char *r = full + strlen(base);
            while (*r == '/' || *r == '\\') r++;
            strncpy(rel, r, sizeof(rel) - 1); rel[sizeof(rel) - 1] = '\0';
            to_vpath(vpath, sizeof(vpath), rel);
            pack_one(pak, full, vpath, n_ok, n_err);
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);

#else
    DIR *d = opendir(dir);
    struct dirent *ent;
    struct stat st;
    char full[4096], vpath[4096];

    if (!d) {
        fprintf(stderr, "Warning: cannot open directory '%s'\n", dir);
        return;
    }
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
            continue;
        snprintf(full, sizeof(full), "%s/%s", dir, ent->d_name);
        if (stat(full, &st) != 0) continue;

        if (S_ISDIR(st.st_mode)) {
            pack_dir(pak, full, base, n_ok, n_err);
        } else if (S_ISREG(st.st_mode)) {
            const char *rel = full + strlen(base);
            while (*rel == '/') rel++;
            to_vpath(vpath, sizeof(vpath), rel);
            pack_one(pak, full, vpath, n_ok, n_err);
        }
    }
    closedir(d);
#endif
}

/* ================================================================== */
/* Ensure the parent directory of `full` exists before writing to it   */
/* ================================================================== */

static void ensure_parent_dir(const char *full)
{
    char dirbuf[4096];
    char *slash;

    strncpy(dirbuf, full, sizeof(dirbuf) - 1);
    dirbuf[sizeof(dirbuf) - 1] = '\0';
    slash = strrchr(dirbuf, '/');
    if (slash) { *slash = '\0'; mkdirs(dirbuf); }
}

/* ================================================================== */
/* Subcommands                                                         */
/* ================================================================== */

static int cmd_pack(int argc, char **argv)
{
    const char *password = NULL, *output = NULL, *source = NULL;
    int i, n_ok = 0, n_err = 0;
    mbpak_t *pak;

    for (i = 1; i < argc; i++) {
        if ((!strcmp(argv[i], "-p") || !strcmp(argv[i], "--password")) && i + 1 < argc)
            password = argv[++i];
        else if ((!strcmp(argv[i], "-o") || !strcmp(argv[i], "--output")) && i + 1 < argc)
            output = argv[++i];
        else if (argv[i][0] != '-')
            source = argv[i];
    }

    if (!password || !*password) {
        fprintf(stderr, "Error: -p PASSWORD is required (.mbpak is AES-256-GCM only)\n");
        return 1;
    }
    if (!output || !source) {
        fprintf(stderr, "Usage: mbpak pack -p PASSWORD -o ARCHIVE.mbpak SOURCE_DIR\n");
        return 1;
    }

    pak = mbpak_create(output, password);
    if (!pak) {
        fprintf(stderr, "Error: cannot create '%s' (already exists?)\n", output);
        return 1;
    }

    pack_dir(pak, source, source, &n_ok, &n_err);

    if (mbpak_flush(pak) != 0) {
        fprintf(stderr, "Error: commit failed\n");
        mbpak_close(pak);
        return 1;
    }
    mbpak_close(pak);

    printf("\nDone: %d file(s) packed, %d error(s) -> %s\n", n_ok, n_err, output);
    return n_err ? 1 : 0;
}

static int cmd_list(int argc, char **argv)
{
    const char *password = NULL, *archive = NULL;
    const mbpak_entry_t *e;
    mbpak_t *pak;
    int i, n = 0;

    for (i = 1; i < argc; i++) {
        if ((!strcmp(argv[i], "-p") || !strcmp(argv[i], "--password")) && i + 1 < argc)
            password = argv[++i];
        else if (argv[i][0] != '-')
            archive = argv[i];
    }

    if (!password || !archive) {
        fprintf(stderr, "Usage: mbpak list -p PASSWORD ARCHIVE.mbpak\n");
        return 1;
    }

    pak = mbpak_open(archive, password);
    if (!pak) {
        fprintf(stderr, "Error: cannot open '%s' (wrong password or corrupted archive)\n", archive);
        return 1;
    }

    for (e = mbpak_list(pak); e; e = e->next) {
        if (e->flags & MBPAK_FLAG_TOMBSTONE) continue;
        printf("%10llu  %s\n", (unsigned long long)e->size, e->path);
        n++;
    }
    printf("-- %d file(s)\n", n);
    mbpak_close(pak);
    return 0;
}

static int cmd_extract(int argc, char **argv)
{
    const char *password = NULL, *output = NULL, *archive = NULL, *only = NULL;
    const mbpak_entry_t *e;
    mbpak_t *pak;
    int i, n_ok = 0, n_err = 0;

    for (i = 1; i < argc; i++) {
        if ((!strcmp(argv[i], "-p") || !strcmp(argv[i], "--password")) && i + 1 < argc)
            password = argv[++i];
        else if ((!strcmp(argv[i], "-o") || !strcmp(argv[i], "--output")) && i + 1 < argc)
            output = argv[++i];
        else if (argv[i][0] != '-') {
            if (!archive) archive = argv[i]; else only = argv[i];
        }
    }

    if (!password || !output || !archive) {
        fprintf(stderr, "Usage: mbpak extract -p PASSWORD -o OUT_DIR ARCHIVE.mbpak [VPATH]\n");
        return 1;
    }

    pak = mbpak_open(archive, password);
    if (!pak) {
        fprintf(stderr, "Error: cannot open '%s'\n", archive);
        return 1;
    }

    for (e = mbpak_list(pak); e; e = e->next) {
        char *data; size_t len; char full[4096];

        if (e->flags & MBPAK_FLAG_TOMBSTONE) continue;
        if (only && strcmp(only, e->path) != 0) continue;

        data = mbpak_get(pak, e->path, &len);
        if (!data) {
            fprintf(stderr, "Error: authentication failed for '%s' (tampered data?)\n", e->path);
            n_err++;
            continue;
        }

        snprintf(full, sizeof(full), "%s/%s", output, e->path);
        ensure_parent_dir(full);

        if (write_file(full, data, len) != 0) {
            fprintf(stderr, "Error: cannot write '%s'\n", full);
            n_err++;
        } else {
            printf("x %s\n", e->path);
            n_ok++;
        }
        free(data);
    }

    mbpak_close(pak);
    printf("\nDone: %d extracted, %d error(s)\n", n_ok, n_err);
    return n_err ? 1 : 0;
}

static int cmd_put(int argc, char **argv)
{
    const char *password = NULL, *archive = NULL, *vpath = NULL, *local = NULL;
    mbpak_t *pak;
    char *data; size_t len;
    int i, pos = 0;

    for (i = 1; i < argc; i++) {
        if ((!strcmp(argv[i], "-p") || !strcmp(argv[i], "--password")) && i + 1 < argc) {
            password = argv[++i];
        } else if (argv[i][0] != '-') {
            if (pos == 0) archive = argv[i];
            else if (pos == 1) vpath = argv[i];
            else local = argv[i];
            pos++;
        }
    }

    if (!password || !archive || !vpath || !local) {
        fprintf(stderr, "Usage: mbpak put -p PASSWORD ARCHIVE.mbpak VPATH LOCAL_FILE\n");
        return 1;
    }

    pak = mbpak_open(archive, password);
    if (!pak) {
        fprintf(stderr, "Error: cannot open '%s'\n", archive);
        return 1;
    }

    data = read_file(local, &len);
    if (!data || !len) {
        fprintf(stderr, "Error: cannot read '%s'\n", local);
        mbpak_close(pak);
        return 1;
    }

    if (mbpak_put(pak, vpath, data, len) != 0 || mbpak_flush(pak) != 0) {
        fprintf(stderr, "Error: write failed\n");
        free(data);
        mbpak_close(pak);
        return 1;
    }
    free(data);
    mbpak_close(pak);

    printf("OK: %s -> %s (%s)\n", local, vpath, archive);
    return 0;
}

static int cmd_get(int argc, char **argv)
{
    const char *password = NULL, *archive = NULL, *vpath = NULL, *output = NULL;
    mbpak_t *pak;
    char *data; size_t len;
    int i, pos = 0;

    for (i = 1; i < argc; i++) {
        if ((!strcmp(argv[i], "-p") || !strcmp(argv[i], "--password")) && i + 1 < argc) {
            password = argv[++i];
        } else if ((!strcmp(argv[i], "-o") || !strcmp(argv[i], "--output")) && i + 1 < argc) {
            output = argv[++i];
        } else if (argv[i][0] != '-') {
            if (pos == 0) archive = argv[i]; else vpath = argv[i];
            pos++;
        }
    }

    if (!password || !archive || !vpath) {
        fprintf(stderr, "Usage: mbpak get -p PASSWORD ARCHIVE.mbpak VPATH [-o LOCAL_FILE]\n");
        return 1;
    }

    pak = mbpak_open(archive, password);
    if (!pak) {
        fprintf(stderr, "Error: cannot open '%s'\n", archive);
        return 1;
    }

    data = mbpak_get(pak, vpath, &len);
    mbpak_close(pak);

    if (!data) {
        fprintf(stderr, "Error: '%s' not found or authentication failed\n", vpath);
        return 1;
    }

    if (output) {
        ensure_parent_dir(output);
        if (write_file(output, data, len) != 0) {
            fprintf(stderr, "Error: cannot write '%s'\n", output);
            free(data);
            return 1;
        }
    } else {
        fwrite(data, 1, len, stdout);
    }
    free(data);
    return 0;
}

static int cmd_rm(int argc, char **argv)
{
    const char *password = NULL, *archive = NULL, *vpath = NULL;
    mbpak_t *pak;
    int i, pos = 0, rc;

    for (i = 1; i < argc; i++) {
        if ((!strcmp(argv[i], "-p") || !strcmp(argv[i], "--password")) && i + 1 < argc) {
            password = argv[++i];
        } else if (argv[i][0] != '-') {
            if (pos == 0) archive = argv[i]; else vpath = argv[i];
            pos++;
        }
    }

    if (!password || !archive || !vpath) {
        fprintf(stderr, "Usage: mbpak rm -p PASSWORD ARCHIVE.mbpak VPATH\n");
        return 1;
    }

    pak = mbpak_open(archive, password);
    if (!pak) {
        fprintf(stderr, "Error: cannot open '%s'\n", archive);
        return 1;
    }

    rc = mbpak_delete(pak, vpath);
    if (rc == 0) rc = mbpak_flush(pak);

    mbpak_close(pak);

    if (rc != 0) {
        fprintf(stderr, "Error: '%s' not found or write failed\n", vpath);
        return 1;
    }
    printf("OK: removed %s (space reclaimed by 'mbpak compact')\n", vpath);
    return 0;
}

static int cmd_compact(int argc, char **argv)
{
    const char *password = NULL, *archive = NULL;
    mbpak_t *pak;
    int i, rc;

    for (i = 1; i < argc; i++) {
        if ((!strcmp(argv[i], "-p") || !strcmp(argv[i], "--password")) && i + 1 < argc)
            password = argv[++i];
        else if (argv[i][0] != '-')
            archive = argv[i];
    }

    if (!password || !archive) {
        fprintf(stderr, "Usage: mbpak compact -p PASSWORD ARCHIVE.mbpak\n");
        return 1;
    }

    pak = mbpak_open(archive, password);
    if (!pak) {
        fprintf(stderr, "Error: cannot open '%s'\n", archive);
        return 1;
    }

    rc = mbpak_compact(pak);
    mbpak_close(pak);

    if (rc != 0) {
        fprintf(stderr, "Error: compact failed (original archive left untouched)\n");
        return 1;
    }
    printf("OK: %s compacted\n", archive);
    return 0;
}

/* ================================================================== */
/* main                                                                 */
/* ================================================================== */

static void print_usage(const char *prog)
{
    printf("Usage: %s COMMAND [OPTIONS]\n\n", prog);
    printf("Commands:\n");
    printf("  pack    -p PASSWORD -o ARCHIVE.mbpak SOURCE_DIR\n");
    printf("  list    -p PASSWORD ARCHIVE.mbpak\n");
    printf("  extract -p PASSWORD -o OUT_DIR ARCHIVE.mbpak [VPATH]\n");
    printf("  put     -p PASSWORD ARCHIVE.mbpak VPATH LOCAL_FILE\n");
    printf("  get     -p PASSWORD ARCHIVE.mbpak VPATH [-o LOCAL_FILE]\n");
    printf("  rm      -p PASSWORD ARCHIVE.mbpak VPATH\n");
    printf("  compact -p PASSWORD ARCHIVE.mbpak\n\n");
    printf("  -h, --help       Show this help message\n");
    printf("  -v, --version    Show version information\n");
}

int main(int argc, char **argv)
{
    const char *cmd;

    if (argc < 2) { print_usage(argv[0]); return 1; }
    cmd = argv[1];

    if (!strcmp(cmd, "pack"))    return cmd_pack(argc - 1, argv + 1);
    if (!strcmp(cmd, "list"))    return cmd_list(argc - 1, argv + 1);
    if (!strcmp(cmd, "extract")) return cmd_extract(argc - 1, argv + 1);
    if (!strcmp(cmd, "put"))     return cmd_put(argc - 1, argv + 1);
    if (!strcmp(cmd, "get"))     return cmd_get(argc - 1, argv + 1);
    if (!strcmp(cmd, "rm"))      return cmd_rm(argc - 1, argv + 1);
    if (!strcmp(cmd, "compact")) return cmd_compact(argc - 1, argv + 1);

    if (!strcmp(cmd, "-h") || !strcmp(cmd, "--help")) { print_usage(argv[0]); return 0; }
    if (!strcmp(cmd, "-v") || !strcmp(cmd, "--version")) {
        printf("mbpak %s\n", VERSION);
        return 0;
    }

    fprintf(stderr, "Unknown command: %s\n\n", cmd);
    print_usage(argv[0]);
    return 1;
}
