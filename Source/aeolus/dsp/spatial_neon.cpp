// Copyright (C) 2026 Aeolus fork contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "spatial_neon.h"
#if AEOLUS_HAS_NEON && defined(AEOLUS_SPATIAL_NEON)
#include <arm_neon.h>

namespace aeolus::dsp {
void processSpatialNeon(DelayLine& delayLine, const BiquadFilter::Spec* filterSpec,
                        BiquadFilter::State* filterState, int leftDelay, int rightDelay,
                        float leftAttenuation, float rightAttenuation,
                        float* in, float* outL, float* outR, int numFrames)
{
    if (numFrames <= 0) return;
    const auto pair = [](float left, float right) -> float32x2_t { return vset_lane_f32(right, vdup_n_f32(left), 1); };
    const auto b0 = pair(filterSpec[0].b[0], filterSpec[1].b[0]);
    const auto b1 = pair(filterSpec[0].b[1], filterSpec[1].b[1]);
    const auto b2 = pair(filterSpec[0].b[2], filterSpec[1].b[2]);
    const auto a1 = pair(filterSpec[0].a[1], filterSpec[1].a[1]);
    const auto a2 = pair(filterSpec[0].a[2], filterSpec[1].a[2]);
    const auto attenuation = pair(leftAttenuation, rightAttenuation);
    auto x0 = pair(filterState[0].x[0], filterState[1].x[0]);
    auto x1 = pair(filterState[0].x[1], filterState[1].x[1]);
    auto y0 = pair(filterState[0].y[0], filterState[1].y[0]);
    auto y1 = pair(filterState[0].y[1], filterState[1].y[1]);
    for (int i = 0; i < numFrames; ++i) {
        delayLine.write(in[i]);
        const auto delayed = pair(delayLine.readNearest(leftDelay), delayLine.readNearest(rightDelay));
        const auto x = vmul_f32(delayed, attenuation);
        auto y = vmul_f32(b0, x);
        y = vadd_f32(y, vmul_f32(b1, x0));
        y = vadd_f32(y, vmul_f32(b2, x1));
        y = vsub_f32(y, vmul_f32(a1, y0));
        y = vsub_f32(y, vmul_f32(a2, y1));
        x1 = x0; x0 = x; y1 = y0; y0 = y;
        // Preserve read-before-write and right-ear-last behavior for aliases.
        outL[i] = vget_lane_f32(y, 0);
        outR[i] = vget_lane_f32(y, 1);
    }
    filterState[0].x[0] = vget_lane_f32(x0, 0); filterState[1].x[0] = vget_lane_f32(x0, 1);
    filterState[0].x[1] = vget_lane_f32(x1, 0); filterState[1].x[1] = vget_lane_f32(x1, 1);
    filterState[0].y[0] = vget_lane_f32(y0, 0); filterState[1].y[0] = vget_lane_f32(y0, 1);
    filterState[0].y[1] = vget_lane_f32(y1, 0); filterState[1].y[1] = vget_lane_f32(y1, 1);
}
}
#endif
