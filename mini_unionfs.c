#define FUSE_USE_VERSION 31
#define _FILE_OFFSET_BITS 64

#include <fuse.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <stdlib.h>

/* ============================================================
   GLOBAL STATE
   ============================================================ */

struct mini_unionfs_state {
    char *lower_dir;
    char *upper_dir;
};

#define STATE ((struct mini_unionfs_state *)fuse_get_context()->private_data)

/* ============================================================
   HELPER: build_path
   ============================================================ */
static void build_path(char *out, size_t sz, const char *base, const char *path)
{
    snprintf(out, sz, "%s%s", base, path);
}

/* ============================================================
   HELPER: build_whiteout_path
   /dir/file.txt  ->  upper_dir/dir/.wh.file.txt
   ============================================================ */
static void build_whiteout_path(char *out, size_t sz, const char *path)
{
    char tmp[4096];
    snprintf(tmp, sizeof(tmp), "%s", path);

    char *slash = strrchr(tmp, '/');
    if (!slash || slash == tmp) {
        /* root-level file: path = "/filename" */
        const char *fname = (slash == tmp) ? slash + 1 : path;
        snprintf(out, sz, "%s/.wh.%s", STATE->upper_dir, fname);
    } else {
        char *fname = slash + 1;
        *slash = '\0';              /* tmp is now the directory part */
        snprintf(out, sz, "%s%s/.wh.%s", STATE->upper_dir, tmp, fname);
    }
}

/* ============================================================
   HELPER: resolve_path
   Returns:  1 = found in upper
             0 = found in lower
            -ENOENT = whiteout'd or missing
   ============================================================ */
static int resolve_path(const char *path, char *out_path)
{
    char wh[4096], upper[4096], lower[4096];

    /* 1. Whiteout check */
    build_whiteout_path(wh, sizeof(wh), path);
    if (access(wh, F_OK) == 0)
        return -ENOENT;

    /* 2. Upper layer */
    build_path(upper, sizeof(upper), STATE->upper_dir, path);
    if (access(upper, F_OK) == 0) {
        strncpy(out_path, upper, 4096);
        return 1;
    }

    /* 3. Lower layer */
    build_path(lower, sizeof(lower), STATE->lower_dir, path);
    if (access(lower, F_OK) == 0) {
        strncpy(out_path, lower, 4096);
        return 0;
    }

    return -ENOENT;
}

/* ============================================================
   HELPER: mkdir_p  — create all parent dirs in a path
   ============================================================ */
static void mkdir_p(const char *path)
{
    char tmp[4096];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
    mkdir(tmp, 0755);
}

/* ============================================================
   HELPER: copy_file_to_upper  — Copy-on-Write
   ============================================================ */
static int copy_file_to_upper(const char *path)
{
    char lower[4096], upper[4096];
    build_path(lower, sizeof(lower), STATE->lower_dir, path);
    build_path(upper, sizeof(upper), STATE->upper_dir, path);

    /* Ensure parent directories exist in upper */
    char parent[4096];
    snprintf(parent, sizeof(parent), "%s", upper);
    char *slash = strrchr(parent, '/');
    if (slash) { *slash = '\0'; mkdir_p(parent); }

    int src = open(lower, O_RDONLY);
    if (src < 0) return -errno;

    struct stat st;
    fstat(src, &st);

    int dst = open(upper, O_WRONLY | O_CREAT | O_TRUNC, st.st_mode);
    if (dst < 0) { close(src); return -errno; }

    char buf[65536];
    ssize_t n;
    while ((n = read(src, buf, sizeof(buf))) > 0) {
        if (write(dst, buf, n) != n) {
            close(src); close(dst);
            return -EIO;
        }
    }

    close(src);
    close(dst);
    return 0;
}

/* ============================================================
   FUSE: getattr  — FUSE3 signature has fuse_file_info*
   ============================================================ */
static int unionfs_getattr(const char *path, struct stat *stbuf,
                            struct fuse_file_info *fi)
{
    (void)fi;
    char rpath[4096];
    int res = resolve_path(path, rpath);
    if (res < 0) return res;

    if (lstat(rpath, stbuf) == -1)
        return -errno;
    return 0;
}

/* ============================================================
   FUSE: readdir  — FUSE3 signature
   ============================================================ */
static int unionfs_readdir(const char *path, void *buf,
                            fuse_fill_dir_t filler, off_t offset,
                            struct fuse_file_info *fi,
                            enum fuse_readdir_flags flags)
{
    (void)offset; (void)fi; (void)flags;

    /* FUSE3: filler takes (buf, name, stat, offset, fill_flags) */
    filler(buf, ".", NULL, 0, 0);
    filler(buf, "..", NULL, 0, 0);

    char upper_dir_path[4096], lower_dir_path[4096];
    build_path(upper_dir_path, sizeof(upper_dir_path), STATE->upper_dir, path);
    build_path(lower_dir_path, sizeof(lower_dir_path), STATE->lower_dir, path);

    /* --- Pass 1: Read upper_dir (takes precedence) --- */
    DIR *du = opendir(upper_dir_path);
    if (du) {
        struct dirent *de;
        while ((de = readdir(du)) != NULL) {
            if (strcmp(de->d_name, ".") == 0 ||
                strcmp(de->d_name, "..") == 0)
                continue;
            if (strncmp(de->d_name, ".wh.", 4) == 0)
                continue;   /* hide whiteout markers */
            filler(buf, de->d_name, NULL, 0, 0);
        }
        closedir(du);
    }

    /* --- Pass 2: Read lower_dir, skip hidden/duplicate entries --- */
    DIR *dl = opendir(lower_dir_path);
    if (dl) {
        struct dirent *de;
        while ((de = readdir(dl)) != NULL) {
            if (strcmp(de->d_name, ".") == 0 ||
                strcmp(de->d_name, "..") == 0)
                continue;

            /* Check for whiteout marker */
            char wh[4096];
            snprintf(wh, sizeof(wh), "%s%s/.wh.%s",
                     STATE->upper_dir, path, de->d_name);
            if (access(wh, F_OK) == 0)
                continue;   /* whiteout'd */

            /* Check if already listed from upper */
            char upper_entry[8192];
            snprintf(upper_entry, sizeof(upper_entry), "%s/%s",
                     upper_dir_path, de->d_name);
            if (access(upper_entry, F_OK) == 0)
                continue;   /* upper version already listed */

            filler(buf, de->d_name, NULL, 0, 0);
        }
        closedir(dl);
    }

    return 0;
}

/* ============================================================
   FUSE: open  — triggers CoW if writing to a lower-only file
   ============================================================ */
static int unionfs_open(const char *path, struct fuse_file_info *fi)
{
    char rpath[4096];
    int loc = resolve_path(path, rpath);
    if (loc < 0) return loc;

    /* Writing to a lower-only file → Copy-on-Write */
    if (loc == 0 &&
        ((fi->flags & O_ACCMODE) == O_WRONLY ||
         (fi->flags & O_ACCMODE) == O_RDWR   ||
         (fi->flags & O_APPEND))) {
        int r = copy_file_to_upper(path);
        if (r < 0) return r;
    }

    return 0;
}

/* ============================================================
   FUSE: read
   ============================================================ */
static int unionfs_read(const char *path, char *buf, size_t size,
                         off_t offset, struct fuse_file_info *fi)
{
    (void)fi;
    char rpath[4096];
    int res = resolve_path(path, rpath);
    if (res < 0) return res;

    int fd = open(rpath, O_RDONLY);
    if (fd < 0) return -errno;

    ssize_t n = pread(fd, buf, size, offset);
    close(fd);
    return n < 0 ? -errno : (int)n;
}

/* ============================================================
   FUSE: write
   ============================================================ */
static int unionfs_write(const char *path, const char *buf, size_t size,
                          off_t offset, struct fuse_file_info *fi)
{
    (void)fi;
    char upper[4096];
    build_path(upper, sizeof(upper), STATE->upper_dir, path);

    int fd = open(upper, O_WRONLY);
    if (fd < 0) return -errno;

    ssize_t n = pwrite(fd, buf, size, offset);
    close(fd);
    return n < 0 ? -errno : (int)n;
}

/* ============================================================
   FUSE: create — always in upper_dir
   ============================================================ */
static int unionfs_create(const char *path, mode_t mode,
                           struct fuse_file_info *fi)
{
    (void)fi;
    char upper[4096];
    build_path(upper, sizeof(upper), STATE->upper_dir, path);

    char parent[4096];
    snprintf(parent, sizeof(parent), "%s", upper);
    char *slash = strrchr(parent, '/');
    if (slash) { *slash = '\0'; mkdir_p(parent); }

    int fd = open(upper, O_CREAT | O_WRONLY | O_TRUNC, mode);
    if (fd < 0) return -errno;
    close(fd);
    return 0;
}

/* ============================================================
   FUSE: unlink — whiteout if in lower, physical delete if in upper
   ============================================================ */
static int unionfs_unlink(const char *path)
{
    char upper[4096];
    build_path(upper, sizeof(upper), STATE->upper_dir, path);

    if (access(upper, F_OK) == 0) {
        if (unlink(upper) < 0) return -errno;
        return 0;
    }

    /* lower-only → create whiteout */
    char lower[4096];
    build_path(lower, sizeof(lower), STATE->lower_dir, path);
    if (access(lower, F_OK) != 0) return -ENOENT;

    char wh[4096];
    build_whiteout_path(wh, sizeof(wh), path);

    char wh_parent[4096];
    snprintf(wh_parent, sizeof(wh_parent), "%s", wh);
    char *slash = strrchr(wh_parent, '/');
    if (slash) { *slash = '\0'; mkdir_p(wh_parent); }

    int fd = open(wh, O_CREAT | O_WRONLY, 0644);
    if (fd < 0) return -errno;
    close(fd);
    return 0;
}

/* ============================================================
   FUSE: mkdir — always in upper_dir
   ============================================================ */
static int unionfs_mkdir(const char *path, mode_t mode)
{
    char upper[4096];
    build_path(upper, sizeof(upper), STATE->upper_dir, path);
    if (mkdir(upper, mode) < 0) return -errno;
    return 0;
}

/* ============================================================
   FUSE: rmdir
   ============================================================ */
static int unionfs_rmdir(const char *path)
{
    char upper[4096];
    build_path(upper, sizeof(upper), STATE->upper_dir, path);

    if (access(upper, F_OK) == 0) {
        if (rmdir(upper) < 0) return -errno;
        return 0;
    }

    /* lower-only → whiteout marker */
    char wh[4096];
    build_whiteout_path(wh, sizeof(wh), path);
    int fd = open(wh, O_CREAT | O_WRONLY, 0644);
    if (fd < 0) return -errno;
    close(fd);
    return 0;
}

/* ============================================================
   FUSE: truncate  — FUSE3 signature has fuse_file_info*
   ============================================================ */
static int unionfs_truncate(const char *path, off_t size,
                             struct fuse_file_info *fi)
{
    (void)fi;
    char upper[4096];
    build_path(upper, sizeof(upper), STATE->upper_dir, path);

    if (access(upper, F_OK) != 0) {
        int r = copy_file_to_upper(path);
        if (r < 0) return r;
    }

    if (truncate(upper, size) < 0) return -errno;
    return 0;
}

/* ============================================================
   FUSE OPERATIONS TABLE
   ============================================================ */
static struct fuse_operations unionfs_oper = {
    .getattr  = unionfs_getattr,
    .readdir  = unionfs_readdir,
    .open     = unionfs_open,
    .read     = unionfs_read,
    .write    = unionfs_write,
    .create   = unionfs_create,
    .unlink   = unionfs_unlink,
    .mkdir    = unionfs_mkdir,
    .rmdir    = unionfs_rmdir,
    .truncate = unionfs_truncate,
};

/* ============================================================
   MAIN
   Usage: ./mini_unionfs <lower_dir> <upper_dir> <mountpoint>
   ============================================================ */
int main(int argc, char *argv[])
{
    if (argc < 4) {
        fprintf(stderr,
            "Usage: %s <lower_dir> <upper_dir> <mountpoint>\n", argv[0]);
        return 1;
    }

    struct mini_unionfs_state *state =
        malloc(sizeof(struct mini_unionfs_state));
    if (!state) { perror("malloc"); return 1; }

    state->lower_dir = realpath(argv[1], NULL);
    state->upper_dir = realpath(argv[2], NULL);

    if (!state->lower_dir || !state->upper_dir) {
        fprintf(stderr, "Error: lower/upper dir not found\n");
        return 1;
    }

    char *fuse_argv[] = { argv[0], argv[3], "-f", "-o", "allow_other,default_permissions", NULL };
    int fuse_argc = 5;

    printf("Mounting: lower=%s  upper=%s  mount=%s\n",
           state->lower_dir, state->upper_dir, argv[3]);

    return fuse_main(fuse_argc, fuse_argv, &unionfs_oper, state);
}
