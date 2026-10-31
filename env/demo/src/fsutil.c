/* fsutil: change files on the root filesystem.  Changes are saved to the disk and survive
 * reboots (see kernel/persist.c).  `make` rebuilds the disk image and resets them.
 *   fsutil write  PATH TEXT...    create or replace PATH with TEXT (newline added), then fsync
 *   fsutil append PATH TEXT...    append TEXT (newline added)
 *   fsutil mkdir  PATH
 *   fsutil rm     PATH            unlink a file or symlink
 *   fsutil rmdir  PATH
 *   fsutil mv     FROM TO
 *   fsutil ln     TARGET LINK     symbolic link
 *   fsutil chmod  OCTAL PATH
 *   fsutil fill   N               rewrite /tmp/fill N times with 1 KiB each (forces log compaction)
 *   fsutil sync                   flush everything to disk */
#include "libchiffon.h"

#define O_APPEND_F 0x400

static int usage(void) {
    chiffon_puts("usage: fsutil write|append PATH TEXT... | mkdir|rm|rmdir PATH | mv FROM TO | ln TARGET LINK | chmod OCTAL PATH | fill N | sync\n");
    return 2;
}

static int put_text(const char *path, int flags, int argc, char **argv) {
    int fd = chiffon_open(path, O_WRONLY | O_CREAT | flags, 0644);
    if (fd < 0) { chiffon_puts("fsutil: cannot open\n"); return 1; }
    for (int i = 0; i < argc; i++) {
        chiffon_write(fd, argv[i], chiffon_strlen(argv[i]));
        chiffon_write(fd, i + 1 < argc ? " " : "\n", 1);
    }
    int r = chiffon_fsync(fd);
    chiffon_close(fd);
    if (r < 0) { chiffon_puts("fsutil: not saved to disk\n"); return 1; }
    return 0;
}

static int parse_octal(const char *s) {
    int v = 0;
    for (; *s >= '0' && *s <= '7'; s++) v = v * 8 + (*s - '0');
    return *s ? -1 : v;
}

static int fill(int n) {
    static char block[1024];
    for (int i = 0; i < (int)sizeof block; i++) block[i] = 'a' + (i % 26);
    for (int k = 0; k < n; k++) {
        block[0] = 'a' + (k % 26);
        int fd = chiffon_open("/tmp/fill", O_WRONLY | O_CREAT | 0x200 /* O_TRUNC */, 0644);
        if (fd < 0) { chiffon_puts("fsutil: fill: cannot open /tmp/fill\n"); return 1; }
        chiffon_write(fd, block, sizeof block);
        chiffon_close(fd);
    }
    chiffon_puts("fsutil: fill done\n");
    return 0;
}

int main(int argc, char **argv, char **envp) {
    (void)envp;
    if (argc < 2) return usage();
    const char *cmd = argv[1];
    int r = 0;
    if (!chiffon_strcmp(cmd, "write") && argc >= 4) r = put_text(argv[2], 0x200 /* O_TRUNC */, argc - 3, argv + 3);
    else if (!chiffon_strcmp(cmd, "append") && argc >= 4) r = put_text(argv[2], O_APPEND_F, argc - 3, argv + 3);
    else if (!chiffon_strcmp(cmd, "mkdir") && argc == 3) r = chiffon_mkdir(argv[2], 0755);
    else if (!chiffon_strcmp(cmd, "rm") && argc == 3) r = chiffon_unlink(argv[2], 0);
    else if (!chiffon_strcmp(cmd, "rmdir") && argc == 3) r = chiffon_unlink(argv[2], 1);
    else if (!chiffon_strcmp(cmd, "mv") && argc == 4) r = chiffon_rename(argv[2], argv[3]);
    else if (!chiffon_strcmp(cmd, "ln") && argc == 4) r = chiffon_symlink(argv[2], argv[3]);
    else if (!chiffon_strcmp(cmd, "chmod") && argc == 4) {
        int m = parse_octal(argv[2]);
        if (m < 0) return usage();
        r = chiffon_chmod(argv[3], m);
    }
    else if (!chiffon_strcmp(cmd, "fill") && argc == 3) {
        int n = 0;
        for (const char *s = argv[2]; *s >= '0' && *s <= '9'; s++) n = n * 10 + (*s - '0');
        r = fill(n);
    }
    else if (!chiffon_strcmp(cmd, "sync") && argc == 2) r = chiffon_sync();
    else return usage();
    if (r < 0) { chiffon_puts("fsutil: failed\n"); return 1; }
    return r ? 1 : 0;
}
