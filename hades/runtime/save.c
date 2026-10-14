#define _DARWIN_C_SOURCE
#include "save.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

static int error(const char *message) {
    fprintf(stderr, "[hades-save] %s (errno=%d)\n", message, errno);
    return -1;
}

static int remove_owned_tree(int parent, const char *name, unsigned depth) {
    if (depth > 64)
        return error("Owned transaction tree exceeds depth limit");
    int fd = openat(parent, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0)
        return errno == ENOENT ? 0 : error("Cannot open owned transaction directory for cleanup");
    DIR *entries = fdopendir(fd);
    if (!entries) {
        close(fd);
        return error("Cannot enumerate transaction cleanup");
    }
    int status = 0;
    struct dirent *entry;
    for (;;) {
        errno = 0;
        entry = readdir(entries);
        if (!entry) {
            if (errno)
                status = -1;
            break;
        }
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
            continue;
        struct stat info;
        if (fstatat(fd, entry->d_name, &info, AT_SYMLINK_NOFOLLOW)
            || (S_ISDIR(info.st_mode) ? remove_owned_tree(fd, entry->d_name, depth + 1)
                                    : unlinkat(fd, entry->d_name, 0))) {
            status = -1;
            break;
        }
    }
    if (closedir(entries))
        status = -1;
    if (!status && unlinkat(parent, name, AT_REMOVEDIR))
        status = -1;
    return status ? error("Cannot remove specifically owned stale transaction tree") : 0;
}

static int copy_file(int source, int target) {
    unsigned char buffer[16384];
    for (;;) {
        ssize_t count = read(source, buffer, sizeof buffer);
        if (count < 0) {
            if (errno == EINTR)
                continue;
            return error("Save snapshot read failed");
        }
        if (!count)
            return fsync(target) ? error("Save snapshot file sync failed") : 0;
        for (ssize_t position = 0; position < count;) {
            ssize_t written = write(target, buffer + position, (size_t)(count - position));
            if (written < 0 && errno == EINTR)
                continue;
            if (written <= 0)
                return error("Save snapshot write failed");
            position += written;
        }
    }
}

static int copy_tree(int source, int target, unsigned depth) {
    if (depth > 64)
        return error("Save snapshot exceeds supported directory depth");
    int duplicate = dup(source);
    DIR *entries = duplicate < 0 ? NULL : fdopendir(duplicate);
    if (!entries) {
        if (duplicate >= 0)
            close(duplicate);
        return error("Cannot enumerate committed save snapshot");
    }
    rewinddir(entries);
    int status = 0;
    struct dirent *entry;
    for (;;) {
        errno = 0;
        entry = readdir(entries);
        if (!entry) {
            if (errno)
                status = -1;
            break;
        }
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
            continue;
        struct stat info;
        if (fstatat(source, entry->d_name, &info, AT_SYMLINK_NOFOLLOW)
            || (!S_ISDIR(info.st_mode) && (!S_ISREG(info.st_mode) || info.st_nlink != 1))) {
            status = error("Save snapshot contains unsupported/symlink/hardlinked entry");
            break;
        }
        int input = openat(source, entry->d_name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK
                           | (S_ISDIR(info.st_mode) ? O_DIRECTORY : 0));
        int output = -1;
        if (input >= 0) {
            struct stat opened;
            if (fstat(input,&opened) || opened.st_dev!=info.st_dev || opened.st_ino!=info.st_ino
                || opened.st_mode!=info.st_mode || (!S_ISDIR(opened.st_mode) && opened.st_nlink!=1)) {
                close(input);
                input=-1;
                errno=ESTALE;
            }
        }
        if (input >= 0) {
            if (S_ISDIR(info.st_mode)) {
                if (!mkdirat(target, entry->d_name, 0700))
                    output = openat(target, entry->d_name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            } else
                output = openat(target, entry->d_name, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        }
        if (input < 0 || output < 0)
            status = error("Cannot open snapshot source/target");
        else
            status = S_ISDIR(info.st_mode) ? copy_tree(input, output, depth + 1) : copy_file(input, output);
        if (!status && !S_ISDIR(info.st_mode)) {
            struct stat after;
            if (fstat(input,&after) || after.st_size!=info.st_size || after.st_nlink!=1
                || after.st_mtimespec.tv_sec!=info.st_mtimespec.tv_sec || after.st_mtimespec.tv_nsec!=info.st_mtimespec.tv_nsec
                || after.st_ctimespec.tv_sec!=info.st_ctimespec.tv_sec || after.st_ctimespec.tv_nsec!=info.st_ctimespec.tv_nsec)
                status=error("Save snapshot source changed during copy; publish refused");
        }
        if (input >= 0 && close(input))
            status = -1;
        if (output >= 0 && close(output))
            status = -1;
        if (status)
            break;
    }
    if (closedir(entries))
        status = -1;
    if (!status && fsync(target))
        status = -1;
    return status ? error("Save snapshot copy/sync failed; committed version retained") : 0;
}

static int snapshot(int parent, int source, const char *name) {
    if (mkdirat(parent, name, 0700))
        return error("Cannot create new owned transaction snapshot");
    int target = openat(parent, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (target < 0)
        return error("Cannot open new transaction snapshot");
    int status = copy_tree(source, target, 0);
    if (close(target))
        status = -1;
    return status;
}

int hades_save_close(hades_save *save) {
    int status = 0;
    int *fds[] = {&save->work, &save->lease, &save->parent};
    for (unsigned i = 0; i < 3; i++) {
        if (*fds[i] >= 0 && close(*fds[i]))
            status = -1;
        *fds[i] = -1;
    }
    save->poisoned = 0;
    return status ? error("Cannot close owned save session") : 0;
}

int hades_save_open(hades_save *save, int directory) {
    if (save->parent >= 0 || directory < 0)
        return error("Missing namespace or duplicate save session");
    save->parent = dup(directory);
    save->lease = save->parent < 0 ? -1 : openat(save->parent, "transaction.lock",
        O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK, 0600);
    struct stat lease_info;
    if (save->lease < 0 || fstat(save->lease, &lease_info) || !S_ISREG(lease_info.st_mode)
        || lease_info.st_nlink != 1 || flock(save->lease, LOCK_EX | LOCK_NB)) {
        hades_save_close(save);
        return error("Cannot exclusively lease the local save namespace");
    }
    if ((mkdirat(save->parent, "committed", 0700) && errno != EEXIST)
        || remove_owned_tree(save->parent, "working", 0) || remove_owned_tree(save->parent, "pending", 0)) {
        hades_save_close(save);
        return error("Cannot initialize/recover owned save transaction");
    }
    int committed = openat(save->parent, "committed", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    int status = committed < 0 || snapshot(save->parent, committed, "working");
    if (committed >= 0 && close(committed))
        status = -1;
    save->work = status ? -1 : openat(save->parent, "working", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (save->work < 0 || fsync(save->parent)) {
        hades_save_close(save);
        return error("Cannot mount recovered working save snapshot");
    }
    fprintf(stderr, "[hades-save] mounted actual working snapshot; uncommitted previous changes discarded\n");
    return 0;
}

int hades_save_commit(hades_save *save) {
    if (save->parent < 0 || save->work < 0 || save->poisoned)
        return error("Commit refused: missing session or poisoned transaction; committed snapshot retained");
    if (remove_owned_tree(save->parent, "pending", 0) || snapshot(save->parent, save->work, "pending")
        || fsync(save->parent)) {
        save->poisoned = 1;
        return error("Commit snapshot preparation failed; committed version unchanged");
    }
    if (renameatx_np(save->parent, "pending", save->parent, "committed", RENAME_SWAP)) {
        save->poisoned = 1;
        return error("Atomic save publish failed; committed version unchanged");
    }
    if (fsync(save->parent)) {
        save->poisoned = 1;
        return error("Save atomically published, but durability sync failed; outcome must be inspected");
    }
    if (remove_owned_tree(save->parent, "pending", 0) || fsync(save->parent)) {
        save->poisoned = 1;
        return error("Save published; old snapshot cleanup failed");
    }
    fprintf(stderr, "[hades-save] SAVE_COMMIT_OK: complete snapshot atomically swapped and synced\n");
    return 0;
}
