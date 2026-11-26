#include "pocketkv/store.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <cstdint>
#include <cstring>
#include <exception>
#include <iomanip>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

void usage(std::ostream& out) {
    out << "PocketKV 0.1.0 — a small persistent key-value store\n\n"
        << "Usage: pocketkv [--json] DATABASE COMMAND [ARGUMENTS]\n\n"
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
        << "  inspect            Full forensic log, amplification, and histogram report\n"
        << "  snapshot NEW_PATH  Write a compact durable copy without overwriting\n"
        << "  verify             Validate the entire database without repairing it\n"
        << "  repair             Truncate only a structurally incomplete final record\n\n"
        << "Global options: --help, --version, --json\n"
        << "Exit codes: 0 success; 1 missing key; 2 invalid input, database or I/O error.\n";
}

std::string read_standard_input() {
    std::string value;
    std::array<char, 16384> buffer{};
    for (;;) {
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

void write_json_string(std::ostream& out, std::string_view input) {
    constexpr char hex[] = "0123456789abcdef";
    out.put('"');
    for (const char ch : input) {
        const auto byte = static_cast<unsigned char>(ch);
        if (byte == '"') out << "\\\"";
        else if (byte == '\\') out << "\\\\";
        else if (byte == '\b') out << "\\b";
        else if (byte == '\f') out << "\\f";
        else if (byte == '\n') out << "\\n";
        else if (byte == '\r') out << "\\r";
        else if (byte == '\t') out << "\\t";
        else if (byte < 0x20U || byte >= 0x7fU) {
            out << "\\u00" << hex[byte >> 4U] << hex[byte & 15U];
        } else {
            out.put(ch);
        }
    }
    out.put('"');
}

void write_base64_string(std::ostream& out, std::string_view input) {
    constexpr char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    out.put('"');
    std::size_t i = 0;
    while (i + 3 <= input.size()) {
        const std::uint32_t v =
            (static_cast<std::uint32_t>(static_cast<unsigned char>(input[i])) << 16U) |
            (static_cast<std::uint32_t>(static_cast<unsigned char>(input[i + 1])) << 8U) |
            static_cast<std::uint32_t>(static_cast<unsigned char>(input[i + 2]));
        out.put(b64[(v >> 18U) & 63U]);
        out.put(b64[(v >> 12U) & 63U]);
        out.put(b64[(v >> 6U) & 63U]);
        out.put(b64[v & 63U]);
        i += 3;
    }
    const auto rem = input.size() - i;
    if (rem == 1) {
        const std::uint32_t v = static_cast<std::uint32_t>(static_cast<unsigned char>(input[i])) << 16U;
        out.put(b64[(v >> 18U) & 63U]);
        out.put(b64[(v >> 12U) & 63U]);
        out << "==";
    } else if (rem == 2) {
        const std::uint32_t v =
            (static_cast<std::uint32_t>(static_cast<unsigned char>(input[i])) << 16U) |
            (static_cast<std::uint32_t>(static_cast<unsigned char>(input[i + 1])) << 8U);
        out.put(b64[(v >> 18U) & 63U]);
        out.put(b64[(v >> 12U) & 63U]);
        out.put(b64[(v >> 6U) & 63U]);
        out.put('=');
    }
    out.put('"');
}

int missing(bool json_mode, std::string_view command, std::string_view key) {
    if (json_mode) {
        std::cout << "{\"ok\":false,\"command\":";
        write_json_string(std::cout, command);
        std::cout << ",\"found\":false,\"key\":";
        write_json_string(std::cout, key);
        std::cout << "}\n";
        std::cout.flush();
        if (!std::cout) {
            std::cerr << "error: could not write command output\n";
            return 2;
        }
        return 1;
    }
    std::cerr << "Key not found.\n";
    return 1;
}

void write_inspection_json(std::ostream& out, const pocketkv::InspectionReport& r) {
    out << "{\"ok\":true,\"command\":\"inspect\""
        << ",\"file_bytes\":" << r.file_bytes
        << ",\"header_bytes\":" << r.header_bytes
        << ",\"record_count\":" << r.record_count
        << ",\"put_records\":" << r.put_records
        << ",\"erase_records\":" << r.erase_records
        << ",\"live_keys\":" << r.live_keys
        << ",\"superseded_records\":" << r.superseded_records
        << ",\"live_key_bytes\":" << r.live_key_bytes
        << ",\"live_value_bytes\":" << r.live_value_bytes
        << ",\"live_record_bytes\":" << r.live_record_bytes
        << ",\"reclaimable_bytes\":" << r.reclaimable_bytes
        << ",\"space_amplification\":" << std::fixed << std::setprecision(4) << r.space_amplification
        << ",\"key_bytes\":{\"min\":" << r.min_key_bytes << ",\"max\":" << r.max_key_bytes << "}"
        << ",\"value_bytes\":{\"min\":" << r.min_value_bytes << ",\"max\":" << r.max_value_bytes << "}"
        << ",\"value_histogram\":{\"le_64\":" << r.values_le_64
        << ",\"le_1024\":" << r.values_le_1024
        << ",\"gt_1024\":" << r.values_gt_1024 << "}"
        << ",\"largest_key\":";
    write_json_string(out, r.largest_key);
    out << ",\"largest_value_bytes\":" << r.largest_value_bytes << "}\n";
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
    bool json_mode = false;
    std::vector<std::string_view> args;
    args.reserve(static_cast<std::size_t>(argc));
    int arg_index = 1;
    if (arg_index < argc && std::string_view(argv[arg_index]) == "--json") {
        json_mode = true;
        ++arg_index;
    }
    for (; arg_index < argc; ++arg_index) {
        args.emplace_back(argv[arg_index]);
    }
    // Also permit trailing `--json` on zero-argument read/maintenance commands.
    if (!json_mode && args.size() == 3 && args[2] == "--json" &&
        (args[1] == "init" || args[1] == "keys" || args[1] == "stats" || args[1] == "inspect" ||
         args[1] == "verify" || args[1] == "repair")) {
        json_mode = true;
        args.pop_back();
    }
    const int n_args = static_cast<int>(args.size()) + 1;
    if (n_args < 3) {
        usage(std::cerr);
        return 2;
    }
    const std::string_view db_path = args[0];
    const std::string_view command = args[1];
    const bool no_arguments = command == "init" || command == "keys" || command == "stats" ||
                              command == "inspect" || command == "verify" || command == "repair";
    const bool stdin_value = command == "put-stdin";
    const bool snapshot = command == "snapshot";
    const bool one_argument = command == "get" || command == "get-raw" || command == "delete" ||
                              stdin_value || snapshot;
    const bool page = command == "keys-prefix";
    if ((!no_arguments && !one_argument && command != "put" && !page) ||
        (page && (n_args < 4 || n_args > 6)) ||
        (no_arguments && n_args != 3) || (one_argument && n_args != 4) || (command == "put" && n_args != 5)) {
        usage(std::cerr);
        return 2;
    }
    try {
        std::size_t page_limit = 100;
        if (page) {
            if (n_args >= 5) {
                const std::string_view argument = args[3];
                const auto parsed = std::from_chars(argument.data(), argument.data() + argument.size(), page_limit);
                if (parsed.ec != std::errc{} || parsed.ptr != argument.data() + argument.size()) {
                    throw pocketkv::Error(pocketkv::ErrorCode::invalid_argument, "page limit must be an integer from 1 to 1000");
                }
            }
            if (args[2].size() > pocketkv::Store::max_key_bytes ||
                page_limit == 0 || page_limit > pocketkv::Store::max_page_keys ||
                (n_args == 6 && (args[4].empty() || args[4].size() > pocketkv::Store::max_key_bytes))) {
                throw pocketkv::Error(pocketkv::ErrorCode::invalid_argument, "invalid prefix, page limit or cursor");
            }
        }
        if ((one_argument && !snapshot) || command == "put") {
            const std::string_view key = args[2];
            if (key.empty() || key.size() > pocketkv::Store::max_key_bytes) {
                throw pocketkv::Error(pocketkv::ErrorCode::invalid_argument,
                                      "key must contain 1 to 4096 bytes");
            }
        }
        if (command == "put" && args[3].size() > pocketkv::Store::max_value_bytes) {
            throw pocketkv::Error(pocketkv::ErrorCode::invalid_argument,
                                  "value exceeds the 1048576-byte limit");
        }
        const auto input = stdin_value ? read_standard_input() : std::string{};
        pocketkv::Options options;
        options.create_if_missing = command == "init" || command == "put" || stdin_value;
        options.repair_incomplete_tail = command == "repair";

        bool found = true;
        std::string value;
        std::vector<std::string> keys;
        pocketkv::Stats info{};
        pocketkv::InspectionReport report{};
        std::uint64_t repaired = 0;
        {
            pocketkv::Store database(db_path, options);
            if (command == "put" || stdin_value) {
                database.put(args[2], stdin_value ? std::string_view(input) : args[3]);
            } else if (command == "get" || command == "get-raw") {
                auto stored = database.get(args[2]);
                found = stored.has_value();
                if (found) value = std::move(*stored);
            } else if (command == "delete") {
                found = database.erase(args[2]);
            } else if (snapshot) {
                database.snapshot(args[2]);
            } else if (command == "keys") {
                keys = database.keys();
            } else if (page) {
                const auto after = n_args == 6 ? std::optional<std::string_view>(args[4]) : std::nullopt;
                keys = database.keys_page(args[2], page_limit, after);
            } else if (command == "repair") {
                repaired = database.repaired_bytes();
            } else if (command == "inspect") {
                report = database.inspect();
            } else if (command == "stats" || command == "verify") {
                info = database.stats();
            }
        }
        if (!found) return missing(json_mode, command, args[2]);
        if (json_mode) {
            if (command == "init") {
                std::cout << "{\"ok\":true,\"command\":\"init\"}\n";
            } else if (command == "put" || stdin_value) {
                const auto len = stdin_value ? input.size() : args[3].size();
                std::cout << "{\"ok\":true,\"command\":";
                write_json_string(std::cout, command);
                std::cout << ",\"key\":";
                write_json_string(std::cout, args[2]);
                std::cout << ",\"bytes\":" << len << "}\n";
            } else if (command == "get" || command == "get-raw") {
                std::cout << "{\"ok\":true,\"command\":";
                write_json_string(std::cout, command);
                std::cout << ",\"found\":true,\"key\":";
                write_json_string(std::cout, args[2]);
                std::cout << ",\"bytes\":" << value.size() << ",\"value\":";
                write_json_string(std::cout, value);
                std::cout << ",\"value_base64\":";
                write_base64_string(std::cout, value);
                std::cout << "}\n";
            } else if (command == "delete") {
                std::cout << "{\"ok\":true,\"command\":\"delete\",\"deleted\":true,\"key\":";
                write_json_string(std::cout, args[2]);
                std::cout << "}\n";
            } else if (snapshot) {
                std::cout << "{\"ok\":true,\"command\":\"snapshot\",\"destination\":";
                write_json_string(std::cout, args[2]);
                std::cout << "}\n";
            } else if (command == "keys" || page) {
                std::cout << "{\"ok\":true,\"command\":";
                write_json_string(std::cout, command);
                std::cout << ",\"count\":" << keys.size() << ",\"keys\":[";
                for (std::size_t i = 0; i < keys.size(); ++i) {
                    if (i > 0) std::cout.put(',');
                    write_json_string(std::cout, keys[i]);
                }
                std::cout << "]}\n";
            } else if (command == "repair") {
                std::cout << "{\"ok\":true,\"command\":\"repair\",\"repaired_bytes\":" << repaired << "}\n";
            } else if (command == "inspect") {
                write_inspection_json(std::cout, report);
            } else if (command == "verify") {
                std::cout << "{\"ok\":true,\"command\":\"verify\",\"key_count\":" << info.key_count
                          << ",\"record_count\":" << info.record_count
                          << ",\"file_bytes\":" << info.file_bytes
                          << ",\"live_value_bytes\":" << info.live_value_bytes << "}\n";
            } else {
                std::cout << "{\"ok\":true,\"command\":\"stats\",\"key_count\":" << info.key_count
                          << ",\"record_count\":" << info.record_count
                          << ",\"file_bytes\":" << info.file_bytes
                          << ",\"live_value_bytes\":" << info.live_value_bytes << "}\n";
            }
            return finish_output();
        }
        if (command == "init") {
            std::cout << "Ready.\n";
        } else if (command == "put" || stdin_value) {
            std::cout << "Saved.\n";
        } else if (command == "get" || command == "get-raw") {
            std::cout.write(value.data(), static_cast<std::streamsize>(value.size()));
            if (command == "get") std::cout.put('\n');
        } else if (command == "delete") {
            std::cout << "Deleted.\n";
        } else if (snapshot) {
            std::cout << "Snapshot saved.\n";
        } else if (command == "keys" || page) {
            for (const auto& key : keys) std::cout << escaped(key) << '\n';
        } else if (command == "repair") {
            std::cout << "Repaired " << repaired << " bytes.\n";
        } else if (command == "verify") {
            std::cout << "OK: " << info.record_count << " records, " << info.key_count << " keys.\n";
        } else if (command == "inspect") {
            std::cout << "file_bytes: " << report.file_bytes << '\n'
                      << "header_bytes: " << report.header_bytes << '\n'
                      << "record_count: " << report.record_count << '\n'
                      << "put_records: " << report.put_records << '\n'
                      << "erase_records: " << report.erase_records << '\n'
                      << "live_keys: " << report.live_keys << '\n'
                      << "superseded_records: " << report.superseded_records << '\n'
                      << "live_key_bytes: " << report.live_key_bytes << '\n'
                      << "live_value_bytes: " << report.live_value_bytes << '\n'
                      << "live_record_bytes: " << report.live_record_bytes << '\n'
                      << "reclaimable_bytes: " << report.reclaimable_bytes << '\n'
                      << "space_amplification: " << std::fixed << std::setprecision(4) << report.space_amplification << '\n'
                      << "value_histogram: <=64B=" << report.values_le_64
                      << " <=1024B=" << report.values_le_1024
                      << " >1024B=" << report.values_gt_1024 << '\n'
                      << "largest_key: " << escaped(report.largest_key)
                      << " (" << report.largest_value_bytes << " bytes)\n";
        } else {
            std::cout << "keys: " << info.key_count << '\n'
                      << "records: " << info.record_count << '\n'
                      << "file_bytes: " << info.file_bytes << '\n'
                      << "live_value_bytes: " << info.live_value_bytes << '\n';
        }
        return finish_output();
    } catch (const pocketkv::Error& error) {
        if (json_mode) {
            std::cerr << "{\"ok\":false,\"error\":";
            write_json_string(std::cerr, error.what());
            std::cerr << "}\n";
        } else {
            std::cerr << "error: " << escaped(error.what()) << '\n';
            if (error.code() == pocketkv::ErrorCode::incomplete_tail)
                std::cerr << "Run the explicit repair command to discard the incomplete final record.\n";
        }
        return 2;
    } catch (const std::exception& error) {
        if (json_mode) {
            std::cerr << "{\"ok\":false,\"error\":";
            write_json_string(std::cerr, error.what());
            std::cerr << "}\n";
        } else {
            std::cerr << "error: " << escaped(error.what()) << '\n';
        }
        return 2;
    }
}
