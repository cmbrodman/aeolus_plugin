// Copyright (C) 2026 Aeolus fork contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "MulticoreProbe.h"

int main(int argc, char** argv)
{
    try {
        if (argc != 3 || std::string(argv[1]) != "--benchmark-multicore"
            || (std::string(argv[2]) != "0" && std::string(argv[2]) != "4")) {
            std::cerr << "Usage: aeolus_multicore_probe --benchmark-multicore {0|4}\n";
            return 2;
        }
        multicore_probe::run(std::stoi(argv[2]));
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
