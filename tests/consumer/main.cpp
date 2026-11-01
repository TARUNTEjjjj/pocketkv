#include <pocketkv/store.hpp>

int main(int argc, char* argv[]) {
    if (argc != 2) return 2;
    {
        pocketkv::Store database(argv[1]);
        database.put("example", "installed library");
        if (database.get("example") != "installed library") return 1;
    }
    {
        pocketkv::Store database(argv[1], {.create_if_missing = false});
        if (database.get("example") != "installed library") return 1;
        if (!database.erase("example") || database.get("example")) return 1;
    }
    return 0;
}
