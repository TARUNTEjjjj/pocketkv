// Keep system declarations and fortified inline functions under their real
// names. Defining these substitutions on the compiler command line can rename
// glibc's inline pread wrapper, which may then bypass our injected function.
#pragma once

#if defined(fsync) || defined(pread) || defined(mkostemp) || defined(fcntl) || \
    defined(writev) || defined(fstat) || defined(geteuid)
#error "Use the forced include header without syscall-name command-line definitions."
#endif

#include <cstddef>
#include <cstdlib>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <unistd.h>

extern "C" int pocketkv_test_fsync(int fd);
extern "C" ssize_t pocketkv_test_pread(int fd, void* output, std::size_t length, off_t offset);
extern "C" int pocketkv_test_mkostemp(char* name, int flags);
extern "C" ssize_t pocketkv_test_writev(int fd, const struct iovec* parts, int count);
extern "C" int pocketkv_test_fstat(int fd, struct stat* metadata);
extern "C" uid_t pocketkv_test_geteuid();

// Force-include this header only while compiling the test copy of store.cpp.
// Keep store.cpp as the primary source file, so GCC applies its ordinary
// translation-unit linkage rules. Production objects never include this header.
#define fsync pocketkv_test_fsync
#define pread pocketkv_test_pread
#define mkostemp pocketkv_test_mkostemp
#define writev pocketkv_test_writev
#define fstat pocketkv_test_fstat
#define geteuid pocketkv_test_geteuid

#ifdef F_FULLFSYNC
// Apple platforms sync with fcntl(F_FULLFSYNC); the wrapper passes every other
// fcntl operation straight through.
extern "C" int pocketkv_test_fcntl(int fd, int operation, int value = 0);
#define fcntl pocketkv_test_fcntl
#endif
