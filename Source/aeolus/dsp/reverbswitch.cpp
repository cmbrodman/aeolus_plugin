// Copyright (C) 2026 Aeolus fork contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "aeolus/dsp/reverbswitch.h"
#include <algorithm>

AEOLUS_NAMESPACE_BEGIN
namespace dsp {
ReverbSwitch::~ReverbSwitch() { reset(); }

void ReverbSwitch::collect()
{
    delete _retired.exchange(nullptr);
}

void ReverbSwitch::reset()
{
    delete _pending.exchange(nullptr);
    collect();
    delete _fading;
    delete _current;
    _fading = _current = nullptr;
    _fadePosition = 0;
}

void ReverbSwitch::prepare(const juce::AudioBuffer<float>& ir, bool zeroDelay, float sampleRate)
{
    auto ready = std::make_unique<Prepared>();
    auto& conv = ready->convolver;
    // IR resources are 44.1 kHz; convolution runs after output resampling.
    juce::AudioBuffer<float> resampled;
    const juce::AudioBuffer<float>* source = &ir;
    if (sampleRate != SAMPLE_RATE_F) {
        const double ratio = double(SAMPLE_RATE_F) / sampleRate;
        const int count = int(std::ceil(ir.getNumSamples() / ratio));
        resampled.setSize(2, count);
        for (int ch = 0; ch < 2; ++ch) {
            juce::LagrangeInterpolator interpolator;
            // Pad the input to allow the interpolator's final lookahead.
            juce::AudioBuffer<float> padded(1, ir.getNumSamples() + 8);
            padded.clear();
            padded.copyFrom(0, 0, ir, ch, 0, ir.getNumSamples());
            interpolator.process(ratio, padded.getReadPointer(0), resampled.getWritePointer(ch), count);
        }
        // Preserve the discrete convolution gain as the number of IR samples
        // changes (the same rate correction used by JUCE's convolver).
        resampled.applyGain(float(ratio));
        source = &resampled;
    }
    conv.setLength(int(source->getNumSamples() / Convolver::BlockSize + 1) * Convolver::BlockSize);
    conv.prepareToPlay(sampleRate, SUB_FRAME_LENGTH);
    conv.setZeroDelay(zeroDelay);
    conv.setIR(*source);
    conv.setDryWet(1.0f, 0.25f, true);
    conv.setNonRealtime(true);
    std::array<float, 512> left {}, right {};
    for (int i = 0; i < 16; ++i) {
        left.fill(0); right.fill(0);
        conv.process(left.data(), right.data(), left.data(), right.data(), left.size());
    }
    ready->fadeSamples = std::max(1, int(sampleRate * 0.05f));
    _tailSeconds.store(float(source->getNumSamples()) / sampleRate);
    // If several changes arrive before the next callback, only the newest is
    // needed. The exchanged pointer belongs exclusively to this thread.
    delete _pending.exchange(ready.release());
}

void ReverbSwitch::process(float* left, float* right, int samples, bool nonRealtime)
{
    if (_fading == nullptr && _retired.load() == nullptr) {
        if (auto* ready = _pending.exchange(nullptr)) {
            _fading = _current;
            _current = ready;
            _current->convolver.setDryWet(1.0f, _wet, true);
            _fadePosition = 0;
        }
    }
    if (_current == nullptr)
        return;
    _current->convolver.setDryWet(1.0f, _wet);
    if (_fading == nullptr && !_current->convolver.isAudible())
        return;
    _current->convolver.setNonRealtime(nonRealtime);
    if (_fading != nullptr) {
        _fading->convolver.setDryWet(1.0f, _wet);
        _fading->convolver.setNonRealtime(nonRealtime);
    }
    for (int offset = 0; offset < samples;) {
        const int count = std::min(int(_oldLeft.size()), samples - offset);
        auto* l = left + offset;
        auto* r = right + offset;
        if (_fading != nullptr) {
            std::copy_n(l, count, _oldLeft.data());
            std::copy_n(r, count, _oldRight.data());
            _fading->convolver.process(_oldLeft.data(), _oldRight.data(),
                                      _oldLeft.data(), _oldRight.data(), count);
        }
        _current->convolver.process(l, r, l, r, count);
        if (_fading != nullptr) {
            for (int i = 0; i < count; ++i) {
                const float mix = std::min(1.0f, float(++_fadePosition) / _current->fadeSamples);
                l[i] = _oldLeft[i] + mix * (l[i] - _oldLeft[i]);
                r[i] = _oldRight[i] + mix * (r[i] - _oldRight[i]);
            }
            if (_fadePosition >= _current->fadeSamples) {
                _retired.store(_fading);
                _fading = nullptr;
            }
        }
        offset += count;
    }
}
}
AEOLUS_NAMESPACE_END
