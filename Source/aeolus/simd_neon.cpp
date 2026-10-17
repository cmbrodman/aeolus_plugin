// Copyright (C) 2026 Aeolus fork contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "simd_neon.h"
#if AEOLUS_HAS_NEON
#include <arm_neon.h>
#include <cassert>

namespace aeolus::neon {
void add(float* out, const float* in, size_t n)
{
    size_t i = 0;
    for (; n-i >= 4; i += 4) vst1q_f32(out+i, vaddq_f32(vld1q_f32(out+i), vld1q_f32(in+i)));
    for (; i < n; ++i) out[i] += in[i];
}
void mul_const_add(float* out, const float* in, float k, size_t n)
{
    size_t i = 0;
    for (; n-i >= 4; i += 4)
        vst1q_f32(out+i, vaddq_f32(vld1q_f32(out+i), vmulq_n_f32(vld1q_f32(in+i), k)));
    for (; i < n; ++i) out[i] += in[i]*k;
}
void add_mul_const(float* out, const float* in, float k, size_t n)
{
    size_t i = 0;
    for (; n-i >= 4; i += 4)
        vst1q_f32(out+i, vmulq_n_f32(vaddq_f32(vld1q_f32(out+i), vld1q_f32(in+i)), k));
    for (; i < n; ++i) out[i] = k*(out[i]+in[i]);
}
void mul_const(float* out, float k, size_t n)
{
    size_t i = 0;
    for (; n-i >= 4; i += 4) vst1q_f32(out+i, vmulq_n_f32(vld1q_f32(out+i), k));
    for (; i < n; ++i) out[i] *= k;
}
float mul_reduce(const float* x, const float* y, size_t n)
{
    float32x4_t accum = vdupq_n_f32(0);
    size_t i = 0;
    for (; n-i >= 4; i += 4)
        accum = vaddq_f32(accum, vmulq_f32(vld1q_f32(x+i), vld1q_f32(y+i)));
    // Match the existing SSE lane-reduction order, with scalar remainder.
    float sum = vaddvq_f32(accum);
    for (; i < n; ++i) sum += x[i]*y[i];
    return sum;
}
template<bool Conjugate>
static void multiply(float* out, const float* a, const float* b, size_t n)
{
    assert(n%2 == 0); // n counts floats, not complex numbers
    size_t i = 0;
    for (; n-i >= 8; i += 8) {
        const auto x = vld2q_f32(a+i), y = vld2q_f32(b+i);
        float32x4x2_t z;
        z.val[0] = vsubq_f32(vmulq_f32(x.val[0], y.val[0]), vmulq_f32(x.val[1], y.val[1]));
        if constexpr (Conjugate)
            z.val[1] = vsubq_f32(vmulq_f32(vnegq_f32(x.val[0]), y.val[1]), vmulq_f32(x.val[1], y.val[0]));
        else
            z.val[1] = vaddq_f32(vmulq_f32(x.val[0], y.val[1]), vmulq_f32(x.val[1], y.val[0]));
        vst2q_f32(out+i, z);
    }
    for (; i < n; i += 2) {
        const float real = a[i]*b[i]-a[i+1]*b[i+1];
        const float imag = Conjugate ? -a[i]*b[i+1]-a[i+1]*b[i] : a[i]*b[i+1]+a[i+1]*b[i];
        out[i] = real; out[i+1] = imag;
    }
}
void complex_mul(float* out, const float* a, const float* b, size_t n) { multiply<false>(out,a,b,n); }
void complex_mul_conj(float* out, const float* a, const float* b, size_t n) { multiply<true>(out,a,b,n); }
void fft_step(float* data, const float* w, size_t n)
{
    assert(n%2 == 0);
    size_t i = 0;
    for (; n-i >= 8; i += 8) {
        const auto x = vld2q_f32(data+n+i), y = vld2q_f32(w+i), first = vld2q_f32(data+i);
        const auto real = vsubq_f32(vmulq_f32(x.val[0],y.val[0]), vmulq_f32(x.val[1],y.val[1]));
        const auto imag = vaddq_f32(vmulq_f32(x.val[0],y.val[1]), vmulq_f32(x.val[1],y.val[0]));
        float32x4x2_t low, high;
        low.val[0] = vaddq_f32(first.val[0],real); low.val[1] = vaddq_f32(first.val[1],imag);
        high.val[0] = vsubq_f32(first.val[0],real); high.val[1] = vsubq_f32(first.val[1],imag);
        vst2q_f32(data+i,low); vst2q_f32(data+n+i,high);
    }
    for (; i < n; i += 2) {
        const float real = data[n+i]*w[i]-data[n+i+1]*w[i+1];
        const float imag = data[n+i]*w[i+1]+data[n+i+1]*w[i];
        data[n+i] = data[i]-real; data[n+i+1] = data[i+1]-imag;
        data[i] += real; data[i+1] += imag;
    }
}
}
#endif
