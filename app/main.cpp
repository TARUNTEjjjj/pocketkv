#include "pocketkv/store.hpp"

#include <exception>
#include <iostream>
#include <string>
#include <string_view>

namespace {

void usage(std::ostream& out) {
    out << "PocketKV 0.1.0 — a small persistent key-value store\n\n"
        << "Usage: pocketkv DATABASE COMMAND [ARGUMENTS]\n\n"
        << "  init               Create a database, or validate an existing one\n"
        << "  put KEY VALUE      Save or replace a value\n"
        << "  get KEY            Print the value followed by a newline\n"
        << "  get-raw KEY        Write the exact stored bytes to standard output\n"
        << "  delete KEY         Delete a key\n"
        << "  keys               List escaped keys in sorted order\n"
        << "  stats              Show record, key, byte and live-value counts\n"
        << "  verify             Validate the entire database without repairing it\n"
        << "  repair             Truncate only a structurally incomplete final record\n\n"
        << "Global options: --help, --version\n"
        << "Exit codes: 0 success; 1 missing key; 2 invalid input or database error.\n";
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
        return 0;
    }
    if (argc == 2 && (std::string_view(argv[1]) == "--help" || std::string_view(argv[1]) == "-h")) {
        usage(std::cout);
        return 0;
    }
    if (argc < 3) {
        usage(std::cerr);
        return 2;
    }
    const std::string_view command(argv[2]);
    const bool no_arguments = command == "init" || command == "keys" || command == "stats" || command == "verify" || command == "repair";
    const bool one_argument = command == "get" || command == "get-raw" || command == "delete";
    if ((!no_arguments && !one_argument && command != "put") ||
        (no_arguments && argc != 3) || (one_argument && argc != 4) || (command == "put" && argc != 5)) {
        usage(std::cerr);
        return 2;
    }
    try {
        pocketkv::Options options;
        options.create_if_missing = command == "init" || command == "put";
        options.repair_incomplete_tail = command == "repair";
        pocketkv::Store database(argv[1], options);
        if (command == "init") {
            std::cout << "Ready.\n";
        } else if (command == "put") {
            database.put(argv[3], argv[4]);
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
        std::cout.flush();
        if (!std::cout) {
            std::cerr << "error: could not write command output\n";
            return 2;
        }
        return 0;
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
