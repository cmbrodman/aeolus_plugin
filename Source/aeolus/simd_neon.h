// Copyright (C) 2026 Aeolus fork contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstddef>

// Advanced SIMD is part of the ARM64 application target. Other architectures
// retain their existing dispatch; no optional ARMv8.2 instructions are used.
#if (defined(__aarch64__) || defined(__arm64__) || defined(_M_ARM64)) && !defined(AEOLUS_DISABLE_NEON)
#define AEOLUS_HAS_NEON 1
namespace aeolus::neon {
void add(float*, const float*, size_t);
void mul_const_add(float*, const float*, float, size_t);
void add_mul_const(float*, const float*, float, size_t);
void mul_const(float*, float, size_t);
float mul_reduce(const float*, const float*, size_t);
void complex_mul(float*, const float*, const float*, size_t);
void complex_mul_conj(float*, const float*, const float*, size_t);
void fft_step(float*, const float*, size_t);
}
#else
#define AEOLUS_HAS_NEON 0
#endif
