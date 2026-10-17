// Copyright (C) 2026 Aeolus fork contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "aeolus/ringbuffer.h"

namespace aeolus {
// Opt-in telemetry. The audio thread only publishes fixed-size records;
// the message thread owns the file and drains the bounded SPSC queue.
class CallbackProfiler : private juce::Timer
{
public:
    struct Sample {
        uint64_t sequence;
        int64_t startNs, durationNs;
        double sampleRate;
        int frames, voices, nativeXruns;
    };
    static std::unique_ptr<CallbackProfiler> create(const juce::String& path);
    ~CallbackProfiler() override;
    void record(int64_t startNs, int64_t durationNs, double rate,
                int frames, int voices, int nativeXruns) noexcept;
private:
    explicit CallbackProfiler(std::unique_ptr<juce::FileOutputStream> output);
    void timerCallback() override;
    std::unique_ptr<juce::FileOutputStream> _output;
    RingBuffer<Sample, 4096> _queue;
    uint64_t _sequence = 0; // audio-thread-owned
    std::atomic<uint64_t> _dropped {0};
};
}
