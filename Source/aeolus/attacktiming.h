// Copyright (C) 2026 Aeolus fork contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <chrono>
#include <cstdint>

namespace aeolus::attacktiming {
enum Stage { Midi, Render, Setup, Trigger, Mono, Spatial, Recycle,
             PipeRender, ChiffRender, SpatialRender, Count };
// Keep fixed counter slots so diagnostic records retain a bounded size.
inline constexpr Stage ChiffSetup = Setup, ChiffClear = Mono, SpatialSetup = Spatial;
struct Counters {
    std::array<int64_t, Count> ns {};
    uint64_t triggers = 0, renderCalls = 0;
    uint64_t voiceCalls = 0, sampledVoiceCalls = 0;
};
#if defined(AEOLUS_ATTACK_TIMING)
inline thread_local Counters counters;
inline thread_local bool enabled = false;
inline thread_local uint64_t voiceCursor = 0;
// Prime stride avoids locking to even/odd voices in the 450-voice test.
// Continuous phase across callbacks walks the serial voice-render order.
inline bool sampleVoice() noexcept {
    if (!enabled) return false;
    ++counters.voiceCalls;
    const bool selected = (voiceCursor++ % 67) == 0;
    if (selected) ++counters.sampledVoiceCalls;
    return selected;
}
inline void begin(bool enable) noexcept { counters = {}; enabled = enable; }
inline Counters snapshot() noexcept { return counters; }
inline void trigger() noexcept { if (enabled) ++counters.triggers; }
inline void render() noexcept { if (enabled) ++counters.renderCalls; }
class Scope {
public:
    explicit Scope(Stage stage, bool selected = true) noexcept : _stage(stage), _enabled(enabled && selected) {
        if (_enabled) _start = std::chrono::steady_clock::now();
    }
    ~Scope() {
        if (_enabled) counters.ns[_stage] += std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - _start).count();
    }
private:
    Stage _stage;
    bool _enabled;
    std::chrono::steady_clock::time_point _start;
};
#else
inline void begin(bool) noexcept {}
inline Counters snapshot() noexcept { return {}; }
inline void trigger() noexcept {}
inline void render() noexcept {}
inline bool sampleVoice() noexcept { return false; }
class Scope { public: explicit Scope(Stage, bool = true) noexcept {} };
#endif
}
