#include "pocketkv/store.hpp"
#include "format.hpp"

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <span>
#include <string>
#include <vector>

namespace {

void check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::abort();
    }
}

struct SplitMix64 {
    std::uint64_t state;
    explicit SplitMix64(std::uint64_t seed) : state(seed) {}
    std::uint64_t next() {
        std::uint64_t z = (state += 0x9e3779b97f4a7c15ULL);
        z = (z ^ (z >> 30U)) * 0xbf58476d1ce4e5b9ULL;
        z = (z ^ (z >> 27U)) * 0x94d049bb133111ebULL;
        return z ^ (z >> 31U);
    }
    std::size_t uniform(std::size_t n) { return static_cast<std::size_t>(next() % n); }
};

std::string random_bytes(SplitMix64& rng, std::size_t min_len, std::size_t max_len) {
    const std::size_t len = min_len + rng.uniform(max_len - min_len + 1);
    std::string out(len, '\0');
    for (std::size_t i = 0; i < len; ++i) {
        out[i] = static_cast<char>(rng.next() & 0xffU);
    }
    return out;
}

void assert_matches_oracle(const pocketkv::Store& store, const std::map<std::string, std::string>& oracle) {
    const auto all_keys = store.keys();
    check(all_keys.size() == oracle.size(), "keys size matches oracle");
    std::uint64_t expected_live_val_bytes = 0;
    std::size_t idx = 0;
    for (const auto& [k, v] : oracle) {
        check(all_keys[idx++] == k, "key order matches std::map oracle");
        const auto got = store.get(k);
        check(got.has_value() && *got == v, "value matches oracle");
        expected_live_val_bytes += v.size();
    }
    const auto st = store.stats();
    check(st.key_count == oracle.size(), "stats key_count matches oracle");
    check(st.live_value_bytes == expected_live_val_bytes, "stats live_value_bytes matches oracle");
    const auto rep = store.inspect();
    check(rep.live_keys == oracle.size(), "inspect live_keys matches oracle");
    check(rep.live_value_bytes == expected_live_val_bytes, "inspect live_value_bytes matches oracle");
    check(rep.put_records + rep.erase_records == rep.record_count, "put + erase == record_count");
    check(rep.superseded_records == rep.put_records - rep.live_keys, "superseded == put - live");
    check(rep.file_bytes >= rep.live_record_bytes, "file_bytes >= live_record_bytes");
    check(rep.reclaimable_bytes == rep.file_bytes - rep.live_record_bytes, "reclaimable accounting");
}

void run_differential_state_machine_fuzz() {
    const auto dir = std::filesystem::temp_directory_path() / "pocketkv-fuzz-diff";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);

    for (const std::uint64_t seed : {0x123456789abcdef0ULL, 0xfeedfacecafebeefULL, 0x20261126ULL}) {
        const auto db_path = dir / ("state-" + std::to_string(seed) + ".db");
        const auto snap_path = dir / ("snap-" + std::to_string(seed) + ".db");
        SplitMix64 rng(seed);
        std::map<std::string, std::string> oracle;
        std::vector<std::string> key_pool;
        for (int i = 0; i < 48; ++i) {
            if (i < 16) key_pool.push_back("k/" + std::to_string(i / 4) + "/" + std::to_string(i));
            else key_pool.push_back(random_bytes(rng, 1, 96));
        }

        auto store = std::make_unique<pocketkv::Store>(db_path);
        for (int step = 0; step < 650; ++step) {
            const auto op = rng.uniform(10);
            const auto& k = key_pool[rng.uniform(key_pool.size())];
            if (op <= 4) {
                const std::string val = (rng.uniform(8) == 0) ? std::string{} : random_bytes(rng, 1, 2048);
                store->put(k, val);
                oracle[k] = val;
            } else if (op <= 6) {
                const bool actual = store->erase(k);
                const bool expected = oracle.erase(k) > 0;
                check(actual == expected, "erase matches oracle");
            } else if (op == 7) {
                const auto prefix = (rng.uniform(2) == 0) ? std::string("k/") : std::string{};
                const std::size_t limit = 1 + rng.uniform(15);
                std::vector<std::string> collected;
                std::optional<std::string> after;
                for (;;) {
                    const auto page = store->keys_page(prefix, limit, after);
                    if (page.empty()) break;
                    collected.insert(collected.end(), page.begin(), page.end());
                    after = page.back();
                    if (page.size() < limit) break;
                }
                std::vector<std::string> expected;
                for (const auto& [ok, _] : oracle) {
                    if (ok.starts_with(prefix)) expected.push_back(ok);
                }
                check(collected == expected, "keys_page matches prefix scan of oracle");
            } else if (op == 8) {
                store.reset();
                store = std::make_unique<pocketkv::Store>(db_path);
                assert_matches_oracle(*store, oracle);
            } else {
                const auto got = store->get(k);
                const auto it = oracle.find(k);
                check(got.has_value() == (it != oracle.end()), "get presence matches oracle");
                if (got.has_value()) check(*got == it->second, "get value matches oracle");
            }
        }
        assert_matches_oracle(*store, oracle);
        store->snapshot(snap_path);
        store.reset();

        pocketkv::Store snap_store(snap_path, pocketkv::Options{.create_if_missing = false});
        assert_matches_oracle(snap_store, oracle);
        const auto snap_report = snap_store.inspect();
        check(snap_report.reclaimable_bytes == 0, "snapshot has zero reclaimable bytes");
        check(snap_report.superseded_records == 0, "snapshot has zero superseded records");
        check(snap_report.erase_records == 0, "snapshot has zero erase records");
    }
    std::filesystem::remove_all(dir);
}

void run_corruption_and_truncation_matrix() {
    const auto dir = std::filesystem::temp_directory_path() / "pocketkv-fuzz-corrupt";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    const auto clean_db = dir / "clean.db";
    {
        pocketkv::Store s(clean_db);
        s.put("alpha", "one");
        s.put("beta", std::string(120, 'B'));
        s.put("gamma", "three");
        s.erase("alpha");
    }
    std::ifstream in(clean_db, std::ios::binary);
    const std::vector<std::uint8_t> raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    check(raw.size() > pocketkv::detail::file_header_size + pocketkv::detail::record_header_size, "fixture size");

    std::set<std::size_t> boundaries{pocketkv::detail::file_header_size};
    std::size_t cursor = pocketkv::detail::file_header_size;
    while (cursor + pocketkv::detail::record_header_size <= raw.size()) {
        const auto hdr = pocketkv::detail::parse_record_header(
            std::span<const std::uint8_t>(raw.data() + cursor, pocketkv::detail::record_header_size));
        cursor += pocketkv::detail::record_header_size + hdr.key_size + hdr.value_size;
        boundaries.insert(cursor);
    }
    check(cursor == raw.size(), "clean fixture ends cleanly on a record boundary");

    for (std::size_t cut = 0; cut < raw.size(); ++cut) {
        const auto broken = dir / "trunc.db";
        std::filesystem::remove(broken);
        {
            std::ofstream out(broken, std::ios::binary | std::ios::trunc);
            out.write(reinterpret_cast<const char*>(raw.data()), static_cast<std::streamsize>(cut));
        }
        const bool is_valid_boundary = boundaries.contains(cut);
        bool threw_strict = false;
        try {
            pocketkv::Store strict_open(broken, pocketkv::Options{.create_if_missing = false, .repair_incomplete_tail = false});
            (void)strict_open.stats();
        } catch (const pocketkv::Error& e) {
            threw_strict = true;
            check(e.code() == pocketkv::ErrorCode::corruption || e.code() == pocketkv::ErrorCode::incomplete_tail,
                  "strict open error code");
        }
        check(threw_strict == !is_valid_boundary,
              is_valid_boundary ? "complete boundary prefix must open strictly"
                                : "truncated database must be rejected without repair");

        if (cut >= pocketkv::detail::file_header_size) {
            pocketkv::Store repaired(broken, pocketkv::Options{.create_if_missing = false, .repair_incomplete_tail = true});
            const auto st = repaired.stats();
            if (is_valid_boundary) {
                check(repaired.repaired_bytes() == 0 && st.file_bytes == cut, "valid boundary needs no repair");
            } else {
                check(repaired.repaired_bytes() > 0 && st.file_bytes < cut && boundaries.contains(st.file_bytes),
                      "tail repair shrank truncated suffix to valid record boundary");
            }
        }
    }

    for (std::size_t byte_idx = 0; byte_idx < std::min<std::size_t>(raw.size(), 96); ++byte_idx) {
        for (int bit = 0; bit < 8; bit += 3) {
            std::vector<std::uint8_t> mutated = raw;
            mutated[byte_idx] ^= static_cast<std::uint8_t>(1U << bit);
            const auto flipped = dir / "bitflip.db";
            std::filesystem::remove(flipped);
            {
                std::ofstream out(flipped, std::ios::binary | std::ios::trunc);
                out.write(reinterpret_cast<const char*>(mutated.data()), static_cast<std::streamsize>(mutated.size()));
            }
            bool detected = false;
            try {
                pocketkv::Store bad(flipped, pocketkv::Options{.create_if_missing = false});
                (void)bad.get("beta");
            } catch (const pocketkv::Error& e) {
                detected = true;
                check(e.code() == pocketkv::ErrorCode::corruption || e.code() == pocketkv::ErrorCode::incomplete_tail,
                      "bitflip error code");
            }
            check(detected, "every header/payload bitflip must be detected by CRC32");
        }
    }
    std::filesystem::remove_all(dir);
}

} // namespace

int main() {
    run_differential_state_machine_fuzz();
    run_corruption_and_truncation_matrix();
    std::cout << "PASS pocketkv differential state-machine fuzz + exhaustive bit-flip/truncation matrix\n";
    return 0;
}
