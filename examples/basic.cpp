#include <pocketkv/store.hpp>
#include <exception>
#include <iostream>

int main(int argc, char* argv[]) {
    if (argc != 2) {
        std::cerr << "Usage: pocketkv-example NEW_DATABASE_PATH\n";
        return 2;
    }
    try {
        pocketkv::Store store(argv[1]);
        store.put("theme", "dark");
        std::cout << "theme = " << store.get("theme").value() << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 2;
    }
}
