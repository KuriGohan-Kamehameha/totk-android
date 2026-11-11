/* Walks argv[1] with the runtime's mod-folder scan (runtime/romfs_scan.h) and prints what it visits; argv[2] = "stop"
   makes the visitor refuse the second file. tests/test_romfs_scan.py builds the tree and checks the lines. */
#include "../games/totk/runtime/romfs_scan.h"
#include <stdio.h>

static int stop_after;
static unsigned long seen;

static int visit(void *user, const char *rel, const char *file, const struct stat *st) {
    (void)user;
    struct stat again;
    if (stat(file, &again) || again.st_ino != st->st_ino || again.st_size != st->st_size) return 3;   /* file is rel's own path */
    if (stop_after && ++seen == 2) return 7;
    printf("F\t%s\t%lld\n", rel, (long long)st->st_size);
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) return 2;
    stop_after = argc > 2 && !strcmp(argv[2], "stop");
    RomfsScanCounts c;
    int rc = romfs_scan_tree(argv[1], visit, NULL, &c);
    printf("C\t%d\t%lu\t%lu\t%lu\t%lu\t%s\n", rc, c.files, c.junk, c.special, c.invalid, rc == -1 ? strerror(errno) : "-");
    return 0;
}
