// Copyright (C) 2026 Aeolus fork contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "aeolus/dsp/convolver.h"
#include <atomic>
#include <array>

AEOLUS_NAMESPACE_BEGIN
namespace dsp {

// prepare/collect/reset run off the audio thread, serialized by the caller.
// process/setWet have one audio-thread owner. Mailboxes transfer sole ownership;
// destruction and worker joins never happen in process().
class ReverbSwitch final
{
public:
    ~ReverbSwitch();
    void prepare(const juce::AudioBuffer<float>& ir, bool zeroDelay, float sampleRate);
    void collect();
    void reset(); // playback must be stopped
    void setWet(float wet) noexcept { _wet = wet; }
    void process(float* left, float* right, int samples, bool nonRealtime);
    float tailSeconds() const noexcept { return _tailSeconds.load(); }

private:
    struct Prepared {
        Convolver convolver;
        int fadeSamples = 1;
    };
    Prepared* _current = nullptr;
    Prepared* _fading = nullptr;
    std::atomic<Prepared*> _pending { nullptr };
    std::atomic<Prepared*> _retired { nullptr };
    std::atomic<float> _tailSeconds { 0.0f };
    float _wet = 0.25f;
    int _fadePosition = 0;
    std::array<float, 256> _oldLeft {}, _oldRight {};
};
}
AEOLUS_NAMESPACE_END
