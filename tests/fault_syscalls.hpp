// Keep system declarations and fortified inline functions under their real
// names. Defining these substitutions on the compiler command line can rename
// glibc's inline pread wrapper, which may then bypass our injected function.
#pragma once

#if defined(fsync) || defined(pread)
#error "Use the forced include header without syscall-name command-line definitions."
#endif

#include <cstddef>
#include <unistd.h>

extern "C" int pocketkv_test_fsync(int fd);
extern "C" ssize_t pocketkv_test_pread(int fd, void* output, std::size_t length, off_t offset);

// Force-include this header only while compiling the test copy of store.cpp.
// Keep store.cpp as the primary source file, so GCC applies its ordinary
// translation-unit linkage rules. Production objects never include this header.
#define fsync pocketkv_test_fsync
#define pread pocketkv_test_pread
