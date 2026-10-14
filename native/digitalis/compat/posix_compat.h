// POSIX/Android declarations the Digitalis translator core uses, mapped onto Windows by
// src/platform_win.cpp. Only what the translator needs; guest files, signals and processes are
// refract_native's.
#pragma once

#include <errno.h>
#include <io.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/types.h>

typedef long long ssize_t;
typedef int pid_t;
typedef long long off64_t;

#ifdef __cplusplus
extern "C" {
#endif

int getpagesize(void);
pid_t getpid(void);
pid_t gettid(void);
// Only the few numbers below; anything else is fatal.
long syscall(long number, ...);
int ftruncate64(int fd, off64_t size);

// x86_64 Linux numbers, as Digitalis' raw-syscall helpers spell them.
#define __NR_write 1
#define __NR_close 3
#define __NR_getpid 39
#define __NR_gettid 186
#define __NR_memfd_create 319
#define SYS_getpid __NR_getpid
#define SYS_gettid __NR_gettid
#define MFD_CLOEXEC 1U

#define PROT_NONE 0
#define PROT_READ 1
#define PROT_WRITE 2
#define PROT_EXEC 4
#define MAP_SHARED 0x01
#define MAP_PRIVATE 0x02
#define MAP_FIXED 0x10
#define MAP_ANONYMOUS 0x20
#define MAP_ANON MAP_ANONYMOUS
#define MAP_32BIT 0x40
#define MAP_NORESERVE 0x4000
#define MAP_POPULATE 0x8000
#define MAP_FIXED_NOREPLACE 0x100000
#define MAP_FAILED ((void*)-1)

void* mmap(void* addr, size_t size, int prot, int flags, int fd, long long offset);
int munmap(void* addr, size_t size);
int mprotect(void* addr, size_t size, int prot);

#ifdef __cplusplus
}
#endif
