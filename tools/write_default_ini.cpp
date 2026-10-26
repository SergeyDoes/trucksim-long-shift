// Writes the default long_shift.ini, so that a release can ship it next to the DLL. The text comes from
// Config.cpp, the same the plugin writes when the file is missing.
// Usage: long_shift_default_ini <output path>

#include <cstdio>
#include <fstream>

#include "Config.h"

int main(int argc, char** argv)
{
    if (argc != 2) {
        std::fprintf(stderr, "usage: long_shift_default_ini <output path>\n");
        return 2;
    }
    std::ofstream out(argv[1], std::ios::binary);
    out << long_shift::default_settings_text();
    out.close();
    if (!out) {
        std::fprintf(stderr, "cannot write %s\n", argv[1]);
        return 1;
    }
    return 0;
}
