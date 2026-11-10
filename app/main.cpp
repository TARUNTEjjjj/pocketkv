#include "pocketkv/store.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <cstring>
#include <exception>
#include <iostream>
#include <string>
#include <string_view>
#include <unistd.h>

namespace {

void usage(std::ostream& out) {
    out << "PocketKV 0.1.0 — a small persistent key-value store\n\n"
        << "Usage: pocketkv DATABASE COMMAND [ARGUMENTS]\n\n"
        << "  init               Create a database, or validate an existing one\n"
        << "  put KEY VALUE      Save or replace a value\n"
        << "  put-stdin KEY      Save exact standard-input bytes (up to 1048576)\n"
        << "  get KEY            Print the value followed by a newline\n"
        << "  get-raw KEY        Write the exact stored bytes to standard output\n"
        << "  delete KEY         Delete a key\n"
        << "  keys               List escaped keys in sorted order\n"
        << "  keys-prefix PREFIX [LIMIT [AFTER]]\n"
        << "                     Page matching keys (default 100, maximum 1000)\n"
        << "  stats              Show record, key, byte and live-value counts\n"
        << "  verify             Validate the entire database without repairing it\n"
        << "  repair             Truncate only a structurally incomplete final record\n\n"
        << "Global options: --help, --version\n"
        << "Exit codes: 0 success; 1 missing key; 2 invalid input, database or I/O error.\n";
}

std::string read_standard_input() {
    std::string value;
    std::array<char, 16384> buffer{};
    for (;;) {
        // Read just one byte beyond the limit, so oversized input is rejected
        // without waiting for EOF or buffering an unbounded producer.
        const auto wanted = std::min(buffer.size(), pocketkv::Store::max_value_bytes - value.size() + 1);
        const auto count = ::read(STDIN_FILENO, buffer.data(), wanted);
        if (count < 0) {
            if (errno == EINTR) continue;
            throw pocketkv::Error(pocketkv::ErrorCode::io,
                                  std::string("could not read standard input: ") + std::strerror(errno));
        }
        if (count == 0) return value;
        const auto received = static_cast<std::size_t>(count);
        if (received > pocketkv::Store::max_value_bytes - value.size()) {
            throw pocketkv::Error(pocketkv::ErrorCode::invalid_argument,
                                  "value exceeds the 1048576-byte limit");
        }
        value.append(buffer.data(), received);
    }
}

int finish_output() {
    // A buffered write can fail only when flushed. Mutations may already have
    // committed successfully even if their acknowledgement cannot be printed.
    std::cout.flush();
    if (!std::cout) {
        std::cerr << "error: could not write command output\n";
        return 2;
    }
    return 0;
}

std::string escaped(std::string_view input) {
    constexpr char hex[] = "0123456789abcdef";
    std::string output;
    for (const char character : input) {
        const auto byte = static_cast<unsigned char>(character);
        if (byte == '\\') output += "\\\\";
        else if (byte >= 32 && byte <= 126) output.push_back(character);
        else {
            output += "\\x";
            output += hex[byte >> 4U];
            output += hex[byte & 15U];
        }
    }
    return output;
}

int missing() {
    std::cerr << "Key not found.\n";
    return 1;
}

} // namespace

int main(int argc, char* argv[]) {
    if (argc == 2 && std::string_view(argv[1]) == "--version") {
        std::cout << "PocketKV 0.1.0\n";
        return finish_output();
    }
    if (argc == 2 && (std::string_view(argv[1]) == "--help" || std::string_view(argv[1]) == "-h")) {
        usage(std::cout);
        return finish_output();
    }
    if (argc < 3) {
        usage(std::cerr);
        return 2;
    }
    const std::string_view command(argv[2]);
    const bool no_arguments = command == "init" || command == "keys" || command == "stats" || command == "verify" || command == "repair";
    const bool stdin_value = command == "put-stdin";
    const bool one_argument = command == "get" || command == "get-raw" || command == "delete" || stdin_value;
    const bool page = command == "keys-prefix";
    if ((!no_arguments && !one_argument && command != "put" && !page) ||
        (page && (argc < 4 || argc > 6)) ||
        (no_arguments && argc != 3) || (one_argument && argc != 4) || (command == "put" && argc != 5)) {
        usage(std::cerr);
        return 2;
    }
    try {
        std::size_t page_limit = 100;
        if (page) {
            if (argc >= 5) {
                const std::string_view argument(argv[4]);
                const auto parsed = std::from_chars(argument.data(), argument.data() + argument.size(), page_limit);
                if (parsed.ec != std::errc{} || parsed.ptr != argument.data() + argument.size()) {
                    throw pocketkv::Error(pocketkv::ErrorCode::invalid_argument, "page limit must be an integer from 1 to 1000");
                }
            }
            if (std::string_view(argv[3]).size() > pocketkv::Store::max_key_bytes ||
                page_limit == 0 || page_limit > pocketkv::Store::max_page_keys ||
                (argc == 6 && (std::string_view(argv[5]).empty() || std::string_view(argv[5]).size() > pocketkv::Store::max_key_bytes))) {
                throw pocketkv::Error(pocketkv::ErrorCode::invalid_argument, "invalid prefix, page limit or cursor");
            }
        }
        // Validate command arguments before opening a Store: a rejected put
        // must not create a database as a side effect of constructor success.
        if (one_argument || command == "put") {
            const std::string_view key(argv[3]);
            if (key.empty() || key.size() > pocketkv::Store::max_key_bytes) {
                throw pocketkv::Error(pocketkv::ErrorCode::invalid_argument,
                                      "key must contain 1 to 4096 bytes");
            }
        }
        if (command == "put" && std::string_view(argv[4]).size() > pocketkv::Store::max_value_bytes) {
            throw pocketkv::Error(pocketkv::ErrorCode::invalid_argument,
                                  "value exceeds the 1048576-byte limit");
        }
        // Consume and validate the whole value before creating or locking the
        // database. Input failures cannot leave a partial mutation.
        const auto input = stdin_value ? read_standard_input() : std::string{};
        pocketkv::Options options;
        options.create_if_missing = command == "init" || command == "put" || stdin_value;
        options.repair_incomplete_tail = command == "repair";
        pocketkv::Store database(argv[1], options);
        if (command == "init") {
            std::cout << "Ready.\n";
        } else if (command == "put" || stdin_value) {
            database.put(argv[3], stdin_value ? std::string_view(input) : std::string_view(argv[4]));
            std::cout << "Saved.\n";
        } else if (command == "get" || command == "get-raw") {
            const auto value = database.get(argv[3]);
            if (!value) return missing();
            std::cout.write(value->data(), static_cast<std::streamsize>(value->size()));
            if (command == "get") std::cout.put('\n');
        } else if (command == "delete") {
            if (!database.erase(argv[3])) return missing();
            std::cout << "Deleted.\n";
        } else if (command == "keys") {
            for (const auto& key : database.keys()) std::cout << escaped(key) << '\n';
        } else if (page) {
            const auto after = argc == 6 ? std::optional<std::string_view>(argv[5]) : std::nullopt;
            for (const auto& key : database.keys_page(argv[3], page_limit, after)) std::cout << escaped(key) << '\n';
        } else if (command == "repair") {
            std::cout << "Repaired " << database.repaired_bytes() << " bytes.\n";
        } else {
            const auto info = database.stats();
            if (command == "verify") {
                std::cout << "OK: " << info.record_count << " records, " << info.key_count << " keys.\n";
            } else {
                std::cout << "keys: " << info.key_count << '\n'
                          << "records: " << info.record_count << '\n'
                          << "file_bytes: " << info.file_bytes << '\n'
                          << "live_value_bytes: " << info.live_value_bytes << '\n';
            }
        }
        return finish_output();
    } catch (const pocketkv::Error& error) {
        std::cerr << "error: " << escaped(error.what()) << '\n';
        if (error.code() == pocketkv::ErrorCode::incomplete_tail)
            std::cerr << "Run the explicit repair command to discard the incomplete final record.\n";
        return 2;
    } catch (const std::exception& error) {
        std::cerr << "error: " << escaped(error.what()) << '\n';
        return 2;
    }
}
