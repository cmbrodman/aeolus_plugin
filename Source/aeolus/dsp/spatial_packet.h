// Copyright (C) 2026 Aeolus fork contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "filter.h"
#include <array>

namespace aeolus::dsp {
// Immutable input, separate mutable result: caller and worker may compute the
// same packet concurrently without sharing filter history or delay storage.
struct SpatialFilterPacket {
    BiquadFilter::Spec spec[2];
    BiquadFilter::State initial[2];
    float input[2][SUB_FRAME_LENGTH];
};
struct SpatialFilterResult {
    BiquadFilter::State state[2];
    float output[2][SUB_FRAME_LENGTH];
};
void processSpatialFilterPacket(const SpatialFilterPacket&, SpatialFilterResult&) noexcept;
}
