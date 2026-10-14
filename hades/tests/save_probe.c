#define _DARWIN_C_SOURCE
#include "../runtime/save.h"
#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static void put(int parent, const char *name, const char *bytes) {
    int fd = openat(parent, name, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0600);
    assert(fd >= 0);
    assert(write(fd, bytes, 8) == 8 && !fsync(fd) && !close(fd));
}

static void expect(int parent, const char *name, const char *bytes) {
    char actual[8];
    int fd = openat(parent, name, O_RDONLY | O_NOFOLLOW);
    assert(fd >= 0 && read(fd, actual, 8) == 8 && !memcmp(actual, bytes, 8) && !close(fd));
}

static hades_save empty(void) { return (hades_save){-1,-1,-1,0}; }

int main(int argc, char **argv) {
    assert(argc == 2);
    int root = open(argv[1], O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    assert(root >= 0);
    hades_save save = empty(), competing = empty();
    assert(!hades_save_open(&save, root));
    assert(hades_save_open(&competing, root));
    put(save.work, "Profile.dat", "old-data");
    put(save.work, "Settings.dat", "old-conf");
    assert(!hades_save_commit(&save));
    put(save.work, "Profile.dat", "new-data");
    save.poisoned = 1;
    assert(hades_save_commit(&save));
    assert(!hades_save_close(&save));
    assert(!hades_save_open(&save, root));
    expect(save.work, "Profile.dat", "old-data");
    expect(save.work, "Settings.dat", "old-conf");
    assert(!hades_save_close(&save));
    for (unsigned phase = 0; phase < 3; phase++) {
        pid_t child = fork();
        assert(child >= 0);
        if (!child) {
            hades_save interrupted = empty();
            assert(!hades_save_open(&interrupted, root));
            put(interrupted.work, "Profile.dat", "new-data");
            if (phase) {
                assert(!mkdirat(root, "pending", 0700));
                int pending = openat(root, "pending", O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
                assert(pending >= 0);
                put(pending, "Profile.dat", "new-data");
                if (phase == 2) {
                    put(pending, "Settings.dat", "new-conf");
                    assert(!fsync(pending));
                    assert(!renameatx_np(root, "pending", root, "committed", RENAME_SWAP));
                }
            }
            _exit(0);
        }
        int status;
        assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && !WEXITSTATUS(status));
        assert(!hades_save_open(&save, root));
        expect(save.work, "Profile.dat", phase == 2 ? "new-data" : "old-data");
        expect(save.work, "Settings.dat", phase == 2 ? "new-conf" : "old-conf");
        assert(!hades_save_close(&save));
    }
    assert(!hades_save_open(&save, root));
    assert(!symlinkat("Profile.dat", save.work, "forbidden-link"));
    assert(hades_save_commit(&save));
    assert(!hades_save_close(&save));
    assert(!hades_save_open(&save, root));
    expect(save.work, "Profile.dat", "new-data");
    assert(faccessat(save.work, "forbidden-link", F_OK, AT_SYMLINK_NOFOLLOW));
    assert(!hades_save_close(&save) && !close(root));
    puts("SAVE_TRANSACTION_OK: actual writes/sync/atomic swap, exclusive lease, poisoned-commit refusal, process-loss recovery before/after publish; not a power-loss proof");
    return 0;
}
