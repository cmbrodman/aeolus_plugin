// Copyright (C) 2026 Aeolus fork contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <atomic>
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace aeolus {
// Single audio writer. UI reads atomic publications and requests a reset;
// it never touches callback-owned accumulators. No allocation or locks.
class PerformanceMeter final {
public:
    struct Snapshot {
        float latest = 0, average = 0, peak = 0;
        uint64_t deadlineMisses = 0;
        int xruns = -1;
        bool recentOverload = false;
    };
    void requestReset() noexcept { _reset.fetch_add(1, std::memory_order_relaxed); }
    void record(int64_t startNs, int64_t durationNs, double sampleRate, int frames,
                bool realtime, int nativeXruns = -1) noexcept
    {
        const auto reset = _reset.load(std::memory_order_relaxed);
        if (reset != _seenReset) {
            _seenReset = reset; _mean = _heldPeak = 0; _misses = 0;
            _lastPeak = _lastMiss = -holdNs; _xrunTotal = 0; _lastNativeXruns = nativeXruns;
        }
        const double seconds = sampleRate > 0 ? frames / sampleRate : 0;
        const float load = seconds > 0 && durationNs >= 0
            ? float(durationNs / (seconds * 1.0e9)) : 0;
        // Time-based smoothing is independent of buffer size and sample rate.
        const float blend = float(1.0 - std::exp(-std::max(0.0, seconds) / 0.3));
        _mean += blend * (load - _mean);
        if (load >= _heldPeak || startNs - _lastPeak >= holdNs) {
            _heldPeak = load; _lastPeak = startNs;
        }
        if (realtime && seconds > 0 && load > 1) { ++_misses; _lastMiss = startNs; }
        if (nativeXruns >= 0) {
            if (_lastNativeXruns >= 0 && nativeXruns > _lastNativeXruns) {
                _xrunTotal += nativeXruns - _lastNativeXruns;
                if (realtime) _lastMiss = startNs;
            }
            _lastNativeXruns = nativeXruns;
        } else {
            _lastNativeXruns = -1; // Unavailable intervals cannot be measured.
        }
        _latest.store(load, std::memory_order_relaxed);
        _average.store(_mean, std::memory_order_relaxed);
        _peak.store(_heldPeak, std::memory_order_relaxed);
        _deadlineMisses.store(_misses, std::memory_order_relaxed);
        _xruns.store(nativeXruns < 0 ? -1 : _xrunTotal, std::memory_order_relaxed);
        _recentOverload.store(realtime && startNs - _lastMiss < holdNs, std::memory_order_relaxed);
    }
    Snapshot snapshot() const noexcept {
        return {_latest.load(), _average.load(), _peak.load(), _deadlineMisses.load(),
                _xruns.load(), _recentOverload.load()};
    }
private:
    static_assert(std::atomic<float>::is_always_lock_free && std::atomic<uint64_t>::is_always_lock_free,
                  "Performance publications must be lock free");
    static constexpr int64_t holdNs = 2000000000LL;
    std::atomic<unsigned> _reset{0};
    unsigned _seenReset = 0;
    float _mean = 0, _heldPeak = 0;
    int64_t _lastPeak = -holdNs, _lastMiss = -holdNs;
    uint64_t _misses = 0;
    int _xrunTotal = 0, _lastNativeXruns = -1;
    std::atomic<float> _latest{0}, _average{0}, _peak{0};
    std::atomic<uint64_t> _deadlineMisses{0};
    std::atomic<int> _xruns{-1};
    std::atomic<bool> _recentOverload{false};
};
}
