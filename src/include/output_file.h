#ifndef GM_OUTPUT_FILE_H
#define GM_OUTPUT_FILE_H

#include <stdlib.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

/* Preserve the invoking user's access to files created under sudo. */
static inline void energy_share_output_file(int fd) {
    if (fd < 0) return;
    mode_t mask = umask(0);
    umask(mask);
    if (fchmod(fd, (mode_t)(0666 & ~mask)) != 0) return;
    const char *uid_text = getenv("SUDO_UID"), *gid_text = getenv("SUDO_GID");
    if (geteuid() != 0 || !uid_text || !gid_text) return;
    char *uid_end, *gid_end;
    unsigned long uid = strtoul(uid_text, &uid_end, 10), gid = strtoul(gid_text, &gid_end, 10);
    if (*uid_text && *gid_text && !*uid_end && !*gid_end) {
        if (fchown(fd, (uid_t)uid, (gid_t)gid) != 0) { /* best effort */ }
    }
}

#endif
