#include <cstdio>

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <itch50-file>\n", argv[0]);
        return 1;
    }
    return 0;
}
