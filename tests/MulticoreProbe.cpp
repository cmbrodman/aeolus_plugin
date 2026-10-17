// Copyright (C) 2026 Aeolus fork contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "SpatialSnapshotProbe.h"

int main(int argc, char** argv)
{
    try {
        const std::string mode = argc > 1 ? argv[1] : "";
        const bool check = mode == "--check-snapshot";
        const bool benchmark = mode == "--benchmark-multicore" || mode == "--benchmark-snapshot";
        const bool cpuArgument = argc == 3 && (std::string(argv[2]) == "0" || std::string(argv[2]) == "4");
        if ((!check && !benchmark) || !(cpuArgument || (check && argc == 2))) {
            std::cerr << "Usage: aeolus_multicore_probe {--benchmark-multicore|--benchmark-snapshot} {0|4}\n"
                         "       aeolus_multicore_probe --check-snapshot [0|4]\n";
            return 2;
        }
        const int firstCpu = cpuArgument ? std::stoi(argv[2]) : -1;
        if (check) {
            multicore_probe::Affinity affinity(firstCpu);
            juce::ScopedNoDenormals noDenormals;
            for (int count = 1; count <= 4; ++count) {
                multicore_probe::verifySnapshot(33, count, firstCpu);
                multicore_probe::verifySnapshot(450, count, firstCpu);
            }
        } else if (mode == "--benchmark-snapshot") {
            multicore_probe::runSnapshot(firstCpu);
        } else {
            multicore_probe::run(firstCpu);
        }
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
