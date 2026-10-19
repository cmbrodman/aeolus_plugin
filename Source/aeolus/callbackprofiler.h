// Copyright (C) 2026 Aeolus fork contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "aeolus/ringbuffer.h"
#include "aeolus/attacktiming.h"
#include <thread>

namespace aeolus {
// Opt-in telemetry. The audio thread only publishes fixed-size records;
// a background writer owns the file and drains the bounded SPSC queue.
class CallbackProfiler
{
public:
    struct Sample {
        uint64_t sequence;
        int64_t startNs, durationNs;
        double sampleRate;
        int frames, voices, nativeXruns;
        attacktiming::Counters attack;
    };
    static std::unique_ptr<CallbackProfiler> create(const juce::String& path);
    ~CallbackProfiler();
    uint64_t getDroppedCount() const noexcept;
    bool hasWriteError() const noexcept;
#if AEOLUS_TESTING
    struct WriterTestState {
        std::atomic<bool> paused {true}, entered {false}, finished {false};
    };
    static std::unique_ptr<CallbackProfiler> createForTesting(
        const juce::String&, std::shared_ptr<WriterTestState>);
#endif
    void record(int64_t startNs, int64_t durationNs, double rate,
                int frames, int voices, int nativeXruns) noexcept;
private:
    struct State;
    explicit CallbackProfiler(std::shared_ptr<State>);
    static void writeRecords(std::shared_ptr<State>) noexcept;
    std::shared_ptr<State> _state;
    std::thread _writer;
    uint64_t _sequence = 0; // audio-thread-owned; producer stopped before destruction
};
}
