// Keep system declarations and fortified inline functions under their real
// names. Defining these substitutions on the compiler command line can rename
// glibc's inline pread wrapper, which may then bypass our injected function.
#if defined(fsync) || defined(pread)
#error "Compile fault_store.cpp without syscall-name command-line definitions."
#endif

#include <cstddef>
#include <unistd.h>

extern "C" int pocketkv_test_fsync(int fd);
extern "C" ssize_t pocketkv_test_pread(int fd, void* output, std::size_t length, off_t offset);

// Only the calls in this isolated copy of the storage engine are substituted.
// The test wrappers and the production library use unchanged system headers.
#define fsync pocketkv_test_fsync
#define pread pocketkv_test_pread
#include "../src/store.cpp"
#undef pread
#undef fsync
