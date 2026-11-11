/* Walk of a mod folder that has no manifest (a mod manager's merged romfs/, runtime/romfs.c), shared by the runtime and
   game-data-free regressions. Every regular file is visited under its path relative to the root; folders are entered
   through their parent's descriptor and never through a link, so nothing outside the root is reached: the root itself
   must be a real folder, and a folder swapped for a link during the walk fails the walk instead of being followed. */
#pragma once
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
#include "romfs_path.h"

#define ROMFS_SCAN_PATH 4096   /* longest relative path, with its terminator */
#define ROMFS_SCAN_DEPTH 32    /* folders below the root; the game's RomFS nests 6 deep */

typedef struct {
    unsigned long files;     /* visited */
    unsigned long junk;      /* desktop leftovers, files or folders (romfs_scan_junk) */
    unsigned long special;   /* links, pipes, devices */
    unsigned long invalid;   /* names a RomFS path cannot have (a backslash) */
} RomfsScanCounts;

/* rel: the RomFS path; file: where it is (root + "/" + rel); st: the entry itself, not what a link names. A nonzero
   return stops the walk and is returned by it. */
typedef int (*RomfsScanVisit)(void *user, const char *rel, const char *file, const struct stat *st);

/* What a desktop leaves behind and no game reads: dot entries (.DS_Store, ._name, .git), desktop.ini, Thumbs.db. */
static inline int romfs_scan_junk(const char *name) {
    return name[0] == '.' || !strcasecmp(name, "desktop.ini") || !strcasecmp(name, "Thumbs.db");
}

static inline int romfs_scan_open(int parent, const char *name) {
    return openat(parent, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
}

/* fd: the folder rel names, owned by the walk from here on. rel and path are the walk's two buffers (ROMFS_SCAN_PATH
   and 2 * ROMFS_SCAN_PATH bytes), shared by every level so a deep tree costs no stack. */
static inline int romfs_scan_walk(int fd, const char *root, char *rel, size_t rel_len, char *path, int depth,
                                  RomfsScanVisit visit, void *user, RomfsScanCounts *counts) {
    DIR *dir = fdopendir(fd);
    if (!dir) { int saved = errno; close(fd); errno = saved; return -1; }
    struct dirent *entry;
    int rc = 0;
    while (!rc && (errno = 0, entry = readdir(dir)) != NULL) {
        const char *name = entry->d_name;
        if (!strcmp(name, ".") || !strcmp(name, "..")) continue;
        if (romfs_scan_junk(name)) { counts->junk++; continue; }
        size_t n = strlen(name), at = rel_len ? rel_len + 1 : 0;
        if (at + n >= ROMFS_SCAN_PATH) { errno = ENAMETOOLONG; rc = -1; break; }
        if (rel_len) rel[rel_len] = '/';
        memcpy(rel + at, name, n + 1);
        struct stat st;
        if (fstatat(fd, name, &st, AT_SYMLINK_NOFOLLOW)) rc = -1;
        else if (S_ISDIR(st.st_mode)) {
            int below = depth < ROMFS_SCAN_DEPTH ? romfs_scan_open(fd, name) : (errno = ELOOP, -1);
            rc = below < 0 ? -1 : romfs_scan_walk(below, root, rel, at + n, path, depth + 1, visit, user, counts);
        }
        else if (!S_ISREG(st.st_mode)) counts->special++;
        else if (!romfs_path_valid_relative(rel)) counts->invalid++;
        else if (snprintf(path, 2 * ROMFS_SCAN_PATH, "%s/%s", root, rel) >= 2 * ROMFS_SCAN_PATH) { errno = ENAMETOOLONG; rc = -1; }
        else { counts->files++; rc = visit(user, rel, path, &st); }
        rel[rel_len] = 0;
    }
    if (!rc && errno) rc = -1;   /* readdir failed; a clean end leaves errno 0 */
    int saved = errno;
    closedir(dir);               /* closes fd */
    errno = saved;
    return rc;
}

/* 0 when the whole tree was walked; -1 with errno when the root is not a real folder (a link: ELOOP or ENOTDIR), a
   folder or entry could not be read, the tree nests deeper than ROMFS_SCAN_DEPTH (ELOOP) or a path is too long
   (ENAMETOOLONG); otherwise the visitor's nonzero return. */
static inline int romfs_scan_tree(const char *root, RomfsScanVisit visit, void *user, RomfsScanCounts *counts) {
    char rel[ROMFS_SCAN_PATH] = "", path[2 * ROMFS_SCAN_PATH];
    memset(counts, 0, sizeof *counts);
    int fd = romfs_scan_open(AT_FDCWD, root);
    if (fd < 0) return -1;
    return romfs_scan_walk(fd, root, rel, 0, path, 0, visit, user, counts);
}
