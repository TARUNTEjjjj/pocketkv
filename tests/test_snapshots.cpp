#include "pocketkv/store.hpp"
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <unistd.h>

namespace {
std::size_t checks = 0;
void require(bool value) { ++checks; if (!value) throw std::runtime_error("snapshot expectation failed"); }
template<class F> void rejects(F operation, pocketkv::ErrorCode code) {
    try { operation(); } catch (const pocketkv::Error& error) { require(error.code() == code); return; }
    throw std::runtime_error("expected snapshot failure");
}
struct Temp {
    std::filesystem::path path;
    Temp() {
        auto name = (std::filesystem::temp_directory_path() / "pocketkv-snapshot-XXXXXX").string();
        if (!::mkdtemp(name.data())) throw std::runtime_error("mkdtemp failed");
        path = name;
    }
    ~Temp() { std::error_code ignored; std::filesystem::remove_all(path, ignored); }
};
std::string bytes(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
void no_temporary_files(const Temp& temp) {
    for (const auto& entry : std::filesystem::directory_iterator(temp.path))
        require(!entry.path().filename().string().starts_with(".pocketkv-"));
}
}

int main() {
    try {
        Temp temp;
        const auto source = temp.path / "source.db", copy = temp.path / "snapshot.db";
        pocketkv::Store db(source);
        db.snapshot(copy);
        { pocketkv::Store empty(copy); require(empty.keys().empty()); require(empty.stats().file_bytes == 16); }
        std::filesystem::remove(copy);
        const std::string key("binary\0key", 10), value("\0\xff\n", 3);
        db.put("changed", "old"); db.put("changed", "new");
        db.put("removed", "gone"); require(db.erase("removed"));
        db.put(key, value); db.put("empty", "");
        db.put("large", std::string(pocketkv::Store::max_value_bytes, 'v'));
        const auto before = bytes(source);
        db.snapshot(copy);
        require(bytes(source) == before);
        {
            pocketkv::Store snapshot(copy);
            require(snapshot.keys() == db.keys());
            for (const auto& saved_key : db.keys()) require(snapshot.get(saved_key) == db.get(saved_key));
            require(snapshot.stats().record_count == db.stats().key_count);
            require(snapshot.stats().file_bytes < db.stats().file_bytes);
            db.put("changed", "later");
            require(snapshot.get("changed") == "new");
        }
        const auto saved = bytes(copy);
        rejects([&] { db.snapshot(copy); }, pocketkv::ErrorCode::io);
        require(bytes(copy) == saved);
        rejects([&] { db.snapshot(source); }, pocketkv::ErrorCode::io);
        const auto link = temp.path / "alias.db";
        std::filesystem::create_symlink(copy, link);
        rejects([&] { db.snapshot(link); }, pocketkv::ErrorCode::io);
        require(bytes(copy) == saved);
        rejects([&] { db.snapshot(""); }, pocketkv::ErrorCode::invalid_argument);
        rejects([&] { db.snapshot(std::string("bad\0name", 8)); }, pocketkv::ErrorCode::invalid_argument);
        rejects([&] { db.snapshot(temp.path / "absent" / "copy.db"); }, pocketkv::ErrorCode::io);
        no_temporary_files(temp);
        // Corrupt a live record after the Store's startup scan. Export must
        // detect it rather than silently copy or publish the damaged value.
        { std::fstream out(source, std::ios::binary | std::ios::in | std::ios::out); out.seekp(-1, std::ios::end); out.put('X'); }
        rejects([&] { db.snapshot(temp.path / "corrupt.db"); }, pocketkv::ErrorCode::corruption);
        require(!std::filesystem::exists(temp.path / "corrupt.db"));
        no_temporary_files(temp);
        std::cout << "PASS " << checks << " snapshot checks\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
