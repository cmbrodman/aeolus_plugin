// Copyright (C) 2026 Aeolus fork contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "spatial_packet.h"
#include "aeolus/simd_neon.h"
#if AEOLUS_HAS_NEON && defined(AEOLUS_SPATIAL_NEON)
#include <arm_neon.h>
#endif
namespace aeolus::dsp {
void processSpatialFilterPacket(const SpatialFilterPacket& packet, SpatialFilterResult& result) noexcept
{
    result.state[0] = packet.initial[0]; result.state[1] = packet.initial[1];
#if AEOLUS_HAS_NEON && defined(AEOLUS_SPATIAL_NEON)
    const auto pair = [](float left, float right) -> float32x2_t { return vset_lane_f32(right, vdup_n_f32(left), 1); };
    const auto b0 = pair(packet.spec[0].b[0], packet.spec[1].b[0]);
    const auto b1 = pair(packet.spec[0].b[1], packet.spec[1].b[1]);
    const auto b2 = pair(packet.spec[0].b[2], packet.spec[1].b[2]);
    const auto a1 = pair(packet.spec[0].a[1], packet.spec[1].a[1]);
    const auto a2 = pair(packet.spec[0].a[2], packet.spec[1].a[2]);
    auto x0 = pair(result.state[0].x[0], result.state[1].x[0]);
    auto x1 = pair(result.state[0].x[1], result.state[1].x[1]);
    auto y0 = pair(result.state[0].y[0], result.state[1].y[0]);
    auto y1 = pair(result.state[0].y[1], result.state[1].y[1]);
    for (int i = 0; i < SUB_FRAME_LENGTH; ++i) {
        const auto x = pair(packet.input[0][i], packet.input[1][i]);
        auto y = vmul_f32(b0, x);
        y = vadd_f32(y, vmul_f32(b1, x0));
        y = vadd_f32(y, vmul_f32(b2, x1));
        y = vsub_f32(y, vmul_f32(a1, y0));
        y = vsub_f32(y, vmul_f32(a2, y1));
        x1 = x0; x0 = x; y1 = y0; y0 = y;
        // Preserve read-before-write and right-ear-last behavior for aliases.
        result.output[0][i] = vget_lane_f32(y, 0);
        result.output[1][i] = vget_lane_f32(y, 1);
    }
    result.state[0].x[0] = vget_lane_f32(x0, 0); result.state[1].x[0] = vget_lane_f32(x0, 1);
    result.state[0].x[1] = vget_lane_f32(x1, 0); result.state[1].x[1] = vget_lane_f32(x1, 1);
    result.state[0].y[0] = vget_lane_f32(y0, 0); result.state[1].y[0] = vget_lane_f32(y0, 1);
    result.state[0].y[1] = vget_lane_f32(y1, 0); result.state[1].y[1] = vget_lane_f32(y1, 1);
#else
    for (int i = 0; i < SUB_FRAME_LENGTH; ++i)
        for (int ear = 0; ear < 2; ++ear)
            result.output[ear][i] = BiquadFilter::tick(packet.spec[ear], result.state[ear], packet.input[ear][i]);
#endif
}
}
