#include <stdint.h>
#include <limits.h>
#include "user_libc.h"
#include "posix_compat.h"
#include "user_api.h"

static int libc_memory_test(void) {
    char overlap[8] = "abcdef";
    (void)memmove(overlap + 2, overlap, 4u);
    if (memcmp(overlap, "ababcd", 6u) != 0) return 1;
    (void)memmove(overlap, overlap + 2, 4u);
    if (memcmp(overlap, "abcdcd", 6u) != 0) return 2;

    if (malloc(0u) != 0 || malloc(UINT32_MAX) != 0) return 3;
    if (atoi("  +42") != 42 || atoi("-42") != -42 ||
        atoi("2147483648") != INT_MAX || atoi("-2147483649") != INT_MIN)
        return 9;
    void *first = malloc(64u);
    void *second = malloc(64u);
    if (!first || !second || first == second ||
        (((uintptr_t)first | (uintptr_t)second) & 7u) != 0u) return 4;

    free(first);
    void *reused = malloc(32u);
    if (reused != first) return 5;
    free(reused);
    free(second);

    void *left = malloc(128u);
    void *middle = malloc(128u);
    void *right = malloc(128u);
    if (!left || !middle || !right) return 6;
    free(left);
    free(middle);
    void *merged = malloc(240u);
    if (merged != left) return 7;
    free(merged);
    free(right);

    if (printf("%u %d %x", 1u, -2, 10u) != 6) return 8;
    putchar('\n');
    return 0;
}

static int directory_stream_test(void) {
    DIR *root;
    DIR *nested;
    struct lion_dirent *entry;
    struct lion_dirent raw_entry;
    uint32_t offset = 1u;
    int saw_readme = 0;
    int saw_subdir = 0;
    int saw_nested = 0;

    if (opendir("/ext2/README.TXT") != 0)
        return 20;
    if (closedir(0) >= 0)
        return 21;
    root = opendir("/ext2");
    if (!root)
        return 22;
    while ((entry = readdir(root)) != 0) {
        if (strcmp(entry->name, "README.TXT") == 0 &&
            entry->type == LIONOS_DIRENT_REGULAR)
            saw_readme = 1;
        if (strcmp(entry->name, "subdir") == 0 &&
            entry->type == LIONOS_DIRENT_DIRECTORY)
            saw_subdir = 1;
    }
    if (!saw_readme || !saw_subdir || readdir(root) != 0)
        return 23;
    if (closedir(root) != 0)
        return 24;

    nested = opendir("/ext2/subdir");
    if (!nested)
        return 25;
    while ((entry = readdir(nested)) != 0) {
        if (strcmp(entry->name, "nested.txt") == 0 &&
            entry->type == LIONOS_DIRENT_REGULAR)
            saw_nested = 1;
    }
    if (!saw_nested || closedir(nested) != 0)
        return 26;

    if (lion_sys_readdir("/ext2", 0, &raw_entry) >= 0)
        return 27;
    if (lion_sys_readdir("/ext2", &offset, &raw_entry) >= 0 ||
        offset != 1u)
        return 28;
    offset = 0u;
    if (lion_sys_readdir("/ext2", &offset, 0) >= 0 ||
        offset != 0u)
        return 29;
    return 0;
}


static int tty_job_control_cleanup(uint32_t child,int result) {
    if(child!=0u&&child!=LIONOS_SYSCALL_ERR){
        if(lion_getstate(child)!=LIONOS_PROCESS_ZOMBIE)
            (void)lion_kill(child,LIONOS_SIG_TERM);
        int32_t ignored_status=0;
        (void)lion_waitpid(child,&ignored_status);
    }
    return result;
}

static int tty_background_io_test(void) {
    struct lion_tty_attributes attributes;
    uint8_t byte=0u;
    static const char output[]="x";
    int32_t status=-1;
    if(lion_tty_claim()!=0||lion_tty_getattr(&attributes)!=0)
        return 47;
    attributes.flags|=LIONOS_TTY_ATTR_TOSTOP;
    if(lion_tty_setattr(&attributes)!=0)
        return 48;

    uint32_t child=lion_fork();
    if(child==0u){
        uint32_t pid=lion_getpid();
        if(lion_setpgid(0u,0u)!=0)
            lion_exit_code(49u);
        if(lion_fread(0,&byte,1u)!=-1)
            lion_exit_code(50u);
        if(lion_fwrite(1,output,1u)!=-1)
            lion_exit_code(51u);
        uint32_t pending=lion_sigpending(pid);
        if((pending&(1u<<(LIONOS_SIG_TTIN-1u)))==0u||
           (pending&(1u<<(LIONOS_SIG_TTOU-1u)))==0u)
            lion_exit_code(52u);
        lion_exit_code(0u);
    }
    if(child==LIONOS_SYSCALL_ERR)
        return 53;

    if(lion_waitpid_options(child,&status,LIONOS_WUNTRACED)!=(int32_t)child||
       !LIONOS_WIFSTOPPED(status)||LIONOS_WSTOPSIG(status)!=LIONOS_SIG_TTIN)
        return tty_job_control_cleanup(child,54);
    status=-1;
    if(lion_waitpid_options(child,&status,LIONOS_WNOHANG|LIONOS_WCONTINUED)!=0)
        return tty_job_control_cleanup(child,55);
    if(lion_kill(child,LIONOS_SIG_CONT)!=0)
        return tty_job_control_cleanup(child,56);
    status=-1;
    if(lion_waitpid_options(child,&status,LIONOS_WCONTINUED)!=(int32_t)child||
       !LIONOS_WIFCONTINUED(status))
        return tty_job_control_cleanup(child,57);
    if(lion_waitpid_options(child,&status,LIONOS_WNOHANG|LIONOS_WCONTINUED)!=0)
        return tty_job_control_cleanup(child,58);

    status=-1;
    if(lion_waitpid_options(child,&status,LIONOS_WUNTRACED)!=(int32_t)child||
       !LIONOS_WIFSTOPPED(status)||LIONOS_WSTOPSIG(status)!=LIONOS_SIG_TTOU)
        return tty_job_control_cleanup(child,59);
    if(lion_kill(child,LIONOS_SIG_CONT)!=0)
        return tty_job_control_cleanup(child,60);
    status=-1;
    if(lion_waitpid_options(child,&status,LIONOS_WCONTINUED)!=(int32_t)child||
       !LIONOS_WIFCONTINUED(status))
        return tty_job_control_cleanup(child,61);
    status=-1;
    if(lion_waitpid(child,&status)!=(int32_t)child||status!=0)
        return tty_job_control_cleanup(child,62);

    attributes.flags&=~LIONOS_TTY_ATTR_TOSTOP;
    if(lion_tty_setattr(&attributes)!=0)
        return 63;
    return 0;
}

static int ext2_symlink_syscall_test(void) {
    static const char link_path[] = "/ext2/readme-link";
    static const char expected_target[] = "README.TXT";
    static const char loop_path[] = "/ext2/loop-a";
    static const char intermediate_path[] = "/ext2/subdir-link/nested.txt";
    static const char direct_path[] = "/ext2/subdir/nested.txt";
    struct lion_stat link_status;
    struct lion_stat target_status;
    struct lion_stat loop_status;
    struct lion_stat intermediate_status;
    char target[sizeof(expected_target) - 1u];
    char short_target[5] = {'?', '?', '?', '?', '!'};
    char unterminated_path[64];

    for (uint32_t i = 0u; i < sizeof(unterminated_path); ++i)
        unterminated_path[i] = 'X';

    if (lion_lstat(link_path, &link_status) < 0 ||
        (link_status.flags & LIONOS_STAT_SYMLINK) == 0u ||
        link_status.size != sizeof(expected_target) - 1u)
        return 30;
    if (lion_stat(link_path, &target_status) < 0 ||
        (target_status.flags & LIONOS_STAT_SYMLINK) != 0u)
        return 31;
    if (lion_readlink(link_path, target, sizeof(target)) !=
            (int32_t)(sizeof(expected_target) - 1u) ||
        memcmp(target, expected_target, sizeof(target)) != 0)
        return 32;
    if (lion_readlink(link_path, short_target, 4u) != 4 ||
        memcmp(short_target, expected_target, 4u) != 0 ||
        short_target[4] != '!')
        return 33;
    if (lion_readlink("/ext2/README.TXT", target, sizeof(target)) >= 0)
        return 34;
    if (lion_readlink(link_path, target, 0u) >= 0 ||
        lion_readlink(link_path, 0, 4u) >= 0 ||
        lion_lstat(0, &link_status) >= 0 ||
        lion_lstat(link_path, 0) >= 0)
        return 35;
    if (lion_lstat(unterminated_path, &link_status) >= 0 ||
        lion_readlink(link_path, target, 4097u) >= 0)
        return 38;
    if (lion_readlink(link_path, (char *)(uintptr_t)0xBFFFFFFFu, 4u) >= 0 ||
        lion_lstat(link_path, (struct lion_stat *)(uintptr_t)0xBFFFFFFFu) >= 0)
        return 39;
    if (lion_lstat(loop_path, &loop_status) < 0 ||
        (loop_status.flags & LIONOS_STAT_SYMLINK) == 0u ||
        loop_status.size != 6u || lion_stat(loop_path, &target_status) >= 0)
        return 36;
    if (lion_lstat(intermediate_path, &intermediate_status) < 0 ||
        (intermediate_status.flags & LIONOS_STAT_SYMLINK) != 0u ||
        lion_stat(direct_path, &target_status) < 0 ||
        intermediate_status.size != target_status.size)
        return 37;
    return 0;
}

static int fork_file_description_test(const char *path) {
    static const char first_chunk[] = "LionOS";
    static const char child_chunk[] = " POSIX";
    static const char parent_chunk[] = "-inspi";
    char buffer[7] = {0};
    int32_t fd = lion_open(path, LIONOS_O_READ);
    if (fd < 0) return 40;
    if (lion_fread(fd, buffer, 6u) != 6 ||
        memcmp(buffer, first_chunk, 6u) != 0) {
        (void)lion_close(fd);
        return 41;
    }

    uint32_t child = lion_fork();
    if (child == 0u) {
        char child_buffer[7] = {0};
        int32_t result = lion_fread(fd, child_buffer, 6u);
        int32_t close_result = lion_close(fd);
        if (result != 6 || memcmp(child_buffer, child_chunk, 6u) != 0 ||
            close_result < 0)
            lion_exit_code(42u);
        lion_exit_code(0u);
    }
    if (child == UINT32_MAX) {
        (void)lion_close(fd);
        return 42;
    }

    int32_t child_status = -1;
    if (lion_waitpid(child, &child_status) != (int32_t)child ||
        child_status != 0)
        return 43;

    for (uint32_t i = 0u; i < sizeof(buffer); ++i) buffer[i] = 0;
    if (lion_fread(fd, buffer, 6u) != 6 ||
        memcmp(buffer, parent_chunk, 6u) != 0) {
        (void)lion_close(fd);
        return 44;
    }
    if (lion_close(fd) < 0) return 45;
    if (lion_fread(fd, buffer, 1u) >= 0) return 46;
    return 0;
}

static int waitpid_options_cleanup(uint32_t child, int result) {
    if (child != 0u && child != LIONOS_SYSCALL_ERR) {
        if (lion_getstate(child) != LIONOS_PROCESS_ZOMBIE)
            (void)lion_kill(child, LIONOS_SIG_TERM);
        int32_t ignored_status = 0;
        (void)lion_waitpid(child, &ignored_status);
    }
    return result;
}

static int waitpid_options_test(void) {
    int32_t status = -1;
    uint32_t child = lion_fork();
    if (child == 0u) {
        for (;;) (void)lion_yield();
    }
    if (child == LIONOS_SYSCALL_ERR)
        return 70;
    if (lion_waitpid_options(child, &status, LIONOS_WNOHANG) != 0 ||
        lion_waitpid_options(child, &status, 0x80000000u) != -1)
        return waitpid_options_cleanup(child, 71);
    if (lion_kill(child, LIONOS_SIG_STOP) != 0 ||
        lion_getstate(child) != LIONOS_PROCESS_STOPPED)
        return waitpid_options_cleanup(child, 72);
    if (lion_waitpid_options(child, &status, LIONOS_WNOHANG) != 0 ||
        lion_waitpid_options(child, &status, LIONOS_WUNTRACED) != (int32_t)child ||
        !LIONOS_WIFSTOPPED(status) ||
        LIONOS_WSTOPSIG(status) != LIONOS_SIG_STOP)
        return waitpid_options_cleanup(child, 73);
    if (lion_kill(child, LIONOS_SIG_CONT) != 0 ||
        lion_getstate(child) != LIONOS_PROCESS_READY)
        return waitpid_options_cleanup(child, 74);
    if (lion_kill(child, LIONOS_SIG_STOP) != 0 ||
        lion_kill(child, LIONOS_SIG_CONT) != 0 ||
        lion_waitpid_options(child, &status,
                             LIONOS_WNOHANG | LIONOS_WUNTRACED) != 0)
        return waitpid_options_cleanup(child, 75);
    if (lion_kill(child, LIONOS_SIG_TERM) != 0)
        return waitpid_options_cleanup(child, 76);
    status = -1;
    if (lion_waitpid(child, &status) != (int32_t)child ||
        status != (int32_t)(128u + LIONOS_SIG_TERM))
        return waitpid_options_cleanup(child, 77);
    return 0;
}

int main(void) {
    if (libc_memory_test() != 0) return 19;

    static const char path[] = "userspace-posix-test.txt";
    static const char first[] = "LionOS POSIX-inspired read/write ";
    static const char second[] = "append OK\n";
    static const char expected[] = "LionOS POSIX-inspired read/write append OK\n";
    char buffer[96];
    struct lion_stat status;

    if (lion_posix_open(path, 0u) >= 0)
        return 1;
    if (lion_posix_open(path, LION_POSIX_O_READ | 0x80000000u) >= 0)
        return 2;
    if (lion_posix_read(-1, buffer, 1u) >= 0)
        return 3;
    if (lion_posix_write(-1, buffer, 1u) >= 0)
        return 4;
    if (lion_posix_read(0, 0, 0u) != 0 || lion_posix_write(0, 0, 0u) != 0)
        return 5;

    int32_t fd = lion_posix_open(path, LION_POSIX_O_WRITE);
    if (fd < 0)
        return 6;
    if (lion_posix_write(fd, first, sizeof(first) - 1u) != (int32_t)(sizeof(first) - 1u)) {
        (void)lion_posix_close(fd);
        return 7;
    }
    if (lion_posix_close(fd) < 0)
        return 8;

    fd = lion_posix_open(path, LION_POSIX_O_WRITE | LION_POSIX_O_APPEND);
    if (fd < 0)
        return 9;
    if (lion_posix_write(fd, second, sizeof(second) - 1u) != (int32_t)(sizeof(second) - 1u)) {
        (void)lion_posix_close(fd);
        return 10;
    }
    if (lion_posix_close(fd) < 0)
        return 11;

    int fork_fd_result = fork_file_description_test(path);
    if (fork_fd_result != 0)
        return fork_fd_result;

    if (lion_posix_stat(path, &status) < 0 ||
        status.size != sizeof(expected) - 1u)
        return 12;

    fd = lion_posix_open(path, LION_POSIX_O_READ);
    if (fd < 0)
        return 13;
    int32_t n = lion_posix_read(fd, buffer, sizeof(buffer) - 1u);
    if (n != (int32_t)(sizeof(expected) - 1u)) {
        (void)lion_posix_close(fd);
        return 14;
    }
    buffer[n] = 0;
    if (strcmp(buffer, expected) != 0) {
        (void)lion_posix_close(fd);
        return 15;
    }
    if (lion_posix_close(fd) < 0)
        return 16;
    if (lion_posix_unlink(path) < 0)
        return 17;
    if (lion_posix_stat(path, &status) >= 0)
        return 18;
    int directory_result = directory_stream_test();
    if (directory_result != 0)
        return directory_result;
    int symlink_result = ext2_symlink_syscall_test();
    if (symlink_result != 0)
        return symlink_result;
    int tty_result = tty_background_io_test();
    if (tty_result != 0)
        return tty_result;
    return waitpid_options_test();
}
