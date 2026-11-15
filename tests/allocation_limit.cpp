// Linked into a test build of the CLI: every allocation larger than the byte
// count in POCKETKV_TEST_ALLOCATION_LIMIT fails, as when memory runs out.
// cli_test.py uses it to check that the CLI reports such a failure instead of
// printing partial output and exiting 0. Only builds without sanitizers use it,
// because AddressSanitizer brings its own operator new.
#include <cstddef>
#include <cstdlib>
#include <new>

namespace {

std::size_t allocation_limit() noexcept {
    // Neither getenv nor strtoull allocates, so this is safe inside operator new.
    static const std::size_t limit = [] {
        const char* text = std::getenv("POCKETKV_TEST_ALLOCATION_LIMIT");
        return text == nullptr ? std::size_t{0} : static_cast<std::size_t>(std::strtoull(text, nullptr, 10));
    }();
    return limit;
}

} // namespace

void* operator new(std::size_t size) {
    const auto limit = allocation_limit();
    if (limit != 0 && size > limit) throw std::bad_alloc();
    if (void* memory = std::malloc(size == 0 ? 1 : size)) return memory;
    throw std::bad_alloc();
}
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
