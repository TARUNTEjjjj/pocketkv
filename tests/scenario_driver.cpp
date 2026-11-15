#include "pocketkv/store.hpp"

#include <cerrno>
#include <iostream>
#include <string>
#include <unistd.h>

namespace {
void acknowledge(const std::string& message) {
    // Each message is below PIPE_BUF, so a blocking pipe write is atomic. A
    // racing SIGKILL cannot leave a misleading half-written ACK in the pipe.
    ssize_t written;
    do {
        written = ::write(STDOUT_FILENO, message.data(), message.size());
    } while (written < 0 && errno == EINTR);
    if (written < 0 || static_cast<std::size_t>(written) != message.size()) {
        throw std::runtime_error("could not acknowledge operation");
    }
}
} // namespace

// Kept separate from the CLI: the parent controls when this process writes and
// kills it without running destructors. Every ACK follows Store::put's fsync.
int main(int argc, char* argv[]) {
    if (argc != 2) return 2;
    try {
        pocketkv::Store database(argv[1]);
        acknowledge("READY\n");
        unsigned int sequence = 0;
        char command = 0;
        while (std::cin.get(command)) {
            if (command != 'W' && command != 'R') return 2;
            const unsigned int count = command == 'R' ? 128U : 1U;
            for (unsigned int i = 0; i < count; ++i) {
                const auto key = "job/" + std::to_string(sequence);
                const std::string value(4096, static_cast<char>('a' + sequence % 26U));
                database.put(key, value);
                acknowledge("ACK " + std::to_string(sequence) + '\n');
                ++sequence;
            }
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
