// Copyright (C) 2026 Aeolus fork contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "delay.h"
#include "filter.h"
#include "aeolus/simd_neon.h"

namespace aeolus::dsp {
void processSpatialNeon(DelayLine& delayLine, const BiquadFilter::Spec* filterSpec,
                        BiquadFilter::State* filterState, int leftDelay, int rightDelay,
                        float leftAttenuation, float rightAttenuation,
                        float* in, float* outL, float* outR, int numFrames);
}
