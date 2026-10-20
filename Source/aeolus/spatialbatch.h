// Copyright (C) 2026 Aeolus fork contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "aeolus/dsp/spatial_packet.h"
#include <memory>
namespace aeolus {
// Experimental balanced spatial-filter batches. No callback completion wait.
// All methods except construction/destruction/environment setup have one audio
// caller. Workers touch private immutable packets and separate result storage.
class SpatialBatch final {
public:
    static constexpr int Capacity = 512;
    static std::unique_ptr<SpatialBatch> fromEnvironment();
    explicit SpatialBatch(int workers, int firstCpu = -1, int testDelayUs = 0);
    ~SpatialBatch();
    void begin(int voices) noexcept;
    dsp::SpatialFilterPacket& packet(int index) noexcept;
    void submit(int index) noexcept;
    void dispatch() noexcept;
    const dsp::SpatialFilterResult* result(int index) noexcept;
    struct Stats { size_t adopted = 0, late = 0, local = 0, busy = 0; };
    Stats stats() const noexcept; // Read only by the audio caller or after it stops.
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
}
