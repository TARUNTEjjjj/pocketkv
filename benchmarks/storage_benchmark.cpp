#include <pocketkv/store.hpp>

#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;
namespace fs = std::filesystem;

struct Config {
    std::size_t keys = 64;
    std::size_t rounds = 8;
    std::size_t trials = 5;
    std::size_t reads = 4096;
    std::size_t value_bytes = 64;
    std::optional<fs::path> directory;
};

std::size_t number(std::string_view text, std::size_t maximum) {
    std::size_t value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size() ||
        value == 0 || value > maximum) {
        throw std::invalid_argument("numeric arguments must be positive and within documented limits");
    }
    return value;
}

Config arguments(int argc, char** argv) {
    Config config;
    for (int i = 1; i < argc; ++i) {
        const std::string_view option(argv[i]);
        if (i + 1 == argc) throw std::invalid_argument("missing option value");
        const std::string_view value(argv[++i]);
        if (option == "--keys") config.keys = number(value, 1000000);
        else if (option == "--rounds") config.rounds = number(value, 1000);
        else if (option == "--trials") config.trials = number(value, 1000);
        else if (option == "--reads") config.reads = number(value, 100000000);
        else if (option == "--value-bytes") config.value_bytes = number(value, pocketkv::Store::max_value_bytes);
        else if (option == "--directory") config.directory = value;
        else throw std::invalid_argument("unknown option: " + std::string(option));
    }
    return config;
}

// Only a freshly created private directory is ever removed. An existing user
// database cannot be selected or overwritten by this benchmark.
class TemporaryDirectory {
public:
    explicit TemporaryDirectory(const fs::path& parent)
        : name_((parent / "pocketkv-benchmark-XXXXXX").string()) {
        if (!::mkdtemp(name_.data())) throw std::runtime_error("cannot create benchmark directory");
    }
    ~TemporaryDirectory() { std::error_code ignored; fs::remove_all(name_, ignored); }
    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;
    [[nodiscard]] fs::path file() const { return fs::path(name_) / "data.db"; }
private:
    std::string name_;
};

double milliseconds(Clock::time_point start, Clock::time_point finish) {
    return std::chrono::duration<double, std::milli>(finish - start).count();
}

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

std::string value_for(std::size_t key, std::size_t round, std::size_t bytes) {
    std::string result(bytes, '\0');
    for (std::size_t i = 0; i < bytes; ++i) {
        result[i] = static_cast<char>((key * 17 + round * 131 + i) % 251);
    }
    return result;
}

void run(const Config& config) {
    // Query the system temporary directory only when --directory is absent.
    const fs::path parent = config.directory ? *config.directory : fs::temp_directory_path();
    std::vector<std::string> keys;
    for (std::size_t i = 0; i < config.keys; ++i) keys.push_back("key" + std::to_string(i));
    std::cout << "trial,live_keys,round,records,file_bytes,live_value_bytes,overwrite_fraction,"
                 "value_bytes,put_operations,put_batch_ms,reopen_ms,get_operations,get_batch_ms,"
                 "sync_policy,cache_mode\n" << std::fixed << std::setprecision(6);

    for (std::size_t trial = 1; trial <= config.trials; ++trial) {
        TemporaryDirectory temporary(parent);
        const auto path = temporary.file();
        { pocketkv::Store initialize(path); } // Creation and directory sync are not timed.
        for (std::size_t round = 1; round <= config.rounds; ++round) {
            std::vector<std::string> expected;
            for (std::size_t i = 0; i < config.keys; ++i) {
                expected.push_back(value_for(i, round, config.value_bytes));
            }
            double put_ms = 0;
            {
                pocketkv::Store writer(path, {.create_if_missing = false});
                const auto start = Clock::now();
                for (std::size_t i = 0; i < config.keys; ++i) writer.put(keys[i], expected[i]);
                put_ms = milliseconds(start, Clock::now());
            }

            const auto open_start = Clock::now();
            pocketkv::Store reader(path, {.create_if_missing = false});
            const double reopen_ms = milliseconds(open_start, Clock::now());
            const auto stats = reader.stats();
            require(stats.key_count == config.keys, "live-key count mismatch");
            require(stats.record_count == config.keys * round, "record count mismatch");
            require(stats.live_value_bytes == config.keys * config.value_bytes, "live-value size mismatch");
            require(stats.file_bytes == fs::file_size(path), "file size mismatch");
            for (std::size_t i = 0; i < config.keys; ++i) {
                require(reader.get(keys[i]) == expected[i], "reopened value mismatch");
            }
            // The complete validation above deliberately warms the read workload.
            // Consume every result and check an independently computed checksum
            // after timing, keeping setup and full-value comparisons out of it.
            std::uint64_t expected_checksum = 0;
            for (std::size_t i = 0; i < config.reads; ++i) {
                const auto& value = expected[i % config.keys];
                expected_checksum += value.size() + static_cast<unsigned char>(value.front());
            }
            std::uint64_t checksum = 0;
            const auto read_start = Clock::now();
            for (std::size_t i = 0; i < config.reads; ++i) {
                const auto value = reader.get(keys[i % config.keys]);
                require(value.has_value() && !value->empty(), "timed read returned no value");
                checksum += value->size() + static_cast<unsigned char>(value->front());
            }
            const double get_ms = milliseconds(read_start, Clock::now());
            require(checksum == expected_checksum, "timed-read checksum mismatch");
            const double overwrite_fraction = static_cast<double>(round - 1) / static_cast<double>(round);
            std::cout << trial << ',' << config.keys << ',' << round << ',' << stats.record_count << ','
                      << stats.file_bytes << ',' << stats.live_value_bytes << ',' << overwrite_fraction << ','
                      << config.value_bytes << ',' << config.keys << ',' << put_ms << ',' << reopen_ms << ','
                      << config.reads << ',' << get_ms << ",fsync_per_put,warm\n";
        }
    }
    std::cout.flush();
}
} // namespace

int main(int argc, char** argv) {
    try {
        std::cout.exceptions(std::ios::badbit | std::ios::failbit);
        if (argc == 2 && std::string_view(argv[1]) == "--help") {
            std::cout << "PocketKV storage benchmark (CSV output)\n"
                "  --keys N          live keys (default 64; max 1000000)\n"
                "  --rounds N        full passes of puts (default 8; max 1000)\n"
                "  --trials N        independent databases (default 5; max 1000)\n"
                "  --reads N         warm reads per round (default 4096; max 100000000)\n"
                "  --value-bytes N   bytes per value (default 64; max 1048576)\n"
                "  --directory PATH existing parent on the filesystem to measure\n";
            std::cout.flush();
            return 0;
        }
        run(arguments(argc, argv));
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "benchmark: " << error.what() << '\n';
        return 2;
    }
}
