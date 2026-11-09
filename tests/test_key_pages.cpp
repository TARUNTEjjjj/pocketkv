#include "pocketkv/store.hpp"
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <vector>
#include <unistd.h>

namespace {
std::size_t checks = 0;
void require(bool ok) {
    ++checks;
    if (!ok) throw std::runtime_error("key-page expectation failed");
}
template<class F> void invalid(F operation) {
    try { operation(); }
    catch (const pocketkv::Error& error) { require(error.code() == pocketkv::ErrorCode::invalid_argument); return; }
    require(false);
}
struct Temp {
    std::filesystem::path path;
    Temp() {
        auto pattern = (std::filesystem::temp_directory_path() / "pocketkv-pages-XXXXXX").string();
        if (!::mkdtemp(pattern.data())) throw std::runtime_error("mkdtemp failed");
        path = pattern;
    }
    ~Temp() { std::error_code ignored; std::filesystem::remove_all(path, ignored); }
};
}

int main() {
    try {
        Temp temp;
        std::vector<std::string> keys{"a", "aa", "ab", "b", "\x7f", "\x80", "\xff", "\xff\xff", std::string("a\0z", 3), std::string(4096, 'x')};
        for (int i = 0; i < 65; ++i) keys.push_back("user/" + std::to_string(i));
        std::sort(keys.begin(), keys.end());
        {
            pocketkv::Store db(temp.path / "data.db");
            require(db.keys_page("").empty());
            for (const auto& key : keys) db.put(key, "value");
        }
        pocketkv::Store db(temp.path / "data.db", {.create_if_missing = false});
        const auto before = db.stats();
        // Independent oracle filters the complete key ledger, then takes a
        // page. Production instead seeks directly into the ordered index.
        const std::vector<std::string> prefixes{"", "a", "user/", "missing", "\xff", std::string("a\0", 2), std::string(4096, 'x')};
        for (const auto& prefix : prefixes) {
            for (const std::size_t limit : {1U, 7U, 1000U}) {
                for (const auto& cursor : std::vector<std::optional<std::string>>{std::nullopt, "0", "a", "ac", "user/30", "z", "\xff\xff"}) {
                    std::vector<std::string> expected;
                    for (const auto& key : keys) {
                        if (key.starts_with(prefix) && (!cursor || key > *cursor) && expected.size() < limit) expected.push_back(key);
                    }
                    const auto after = cursor ? std::optional<std::string_view>(*cursor) : std::nullopt;
                    require(db.keys_page(prefix, limit, after) == expected);
                }
                std::vector<std::string> collected;
                std::optional<std::string> cursor;
                for (;;) {
                    const auto page = db.keys_page(prefix, limit, cursor ? std::optional<std::string_view>(*cursor) : std::nullopt);
                    if (page.empty()) break;
                    require(page.size() <= limit);
                    collected.insert(collected.end(), page.begin(), page.end());
                    cursor = page.back();
                }
                std::vector<std::string> expected;
                for (const auto& key : keys) if (key.starts_with(prefix)) expected.push_back(key);
                require(collected == expected);
            }
        }
        invalid([&] { (void)db.keys_page("", 0); });
        invalid([&] { (void)db.keys_page("", pocketkv::Store::max_page_keys + 1); });
        invalid([&] { (void)db.keys_page(std::string(4097, 'x')); });
        invalid([&] { (void)db.keys_page("", 1, ""); });
        invalid([&] { (void)db.keys_page("", 1, std::string(4097, 'x')); });
        require(db.stats().record_count == before.record_count && db.stats().file_bytes == before.file_bytes);
        const auto first = db.keys_page("user/", 1);
        require(first == std::vector<std::string>{"user/0"});
        require(db.erase(first.back()));
        db.put("user/00", "new");
        db.put("user/", "before cursor");
        require(db.keys_page("user/", 2, first.back()) == (std::vector<std::string>{"user/00", "user/1"}));
        std::cout << "PASS " << checks << " key-page checks\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
