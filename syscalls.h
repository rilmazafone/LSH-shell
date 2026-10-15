#ifndef LSH_SYSCALLS_H
#define LSH_SYSCALLS_H

#if !defined(__APPLE__) || !defined(__aarch64__)
#error "LSH targets macOS on Apple silicon only (raw Darwin arm64 syscalls in syscalls.S)"
#endif

#include <sys/types.h>

void    sys_exit(int code) __attribute__((noreturn));

ssize_t sys_read(int fd, void *buf, size_t len);
ssize_t sys_write(int fd, const void *buf, size_t len);

int     sys_open(const char *path, int flags, int mode);
int     sys_close(int fd);

off_t   sys_lseek(int fd, off_t offset, int whence);

int     sys_chdir(const char *path);
pid_t   sys_getpid(void);
int     sys_kill(pid_t pid, int sig);

#endif
