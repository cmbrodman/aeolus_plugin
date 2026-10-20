// Copyright (C) 2026 Aeolus fork contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "aeolus/voice.h"

namespace aeolus {
// Bounded one-to-eight-quantum research snapshot, never a replacement for canonical ownership.
// Construction copies full buffers and may allocate: prepare outside processing.
// capture/render/adopt allocate nothing and copy only bounded read/write windows.
// The constructor's Pipewave must stay immutable and alive for this snapshot's
// lifetime, including rejected/late jobs. The opt-in live mode accepts rebinding
// only to immutable, retained playback generations carried in the voice state.
// One coordinator captures/adopts; one worker renders; external publication is
// required. Never mutate/reuse a snapshot while its worker may still touch it.
class VoiceSnapshot final {
public:
    explicit VoiceSnapshot(const Voice& preparedVoice, bool allowRebind = false);
    bool capture(const Voice&, const juce::Random& pipeRandom,
                 const juce::Random& chiffRandom, unsigned quanta = 1) noexcept;
    void render() noexcept;
    unsigned overMask() const noexcept { return _overMask; }
    // Canonical state must still be the captured state: no local processing,
    // reset, release, retrigger or resource mutation between capture and adopt.
    void adopt(Voice&, juce::Random& pipeRandom, juce::Random& chiffRandom) const noexcept;
    const std::array<float, 16*SUB_FRAME_LENGTH>& output() const noexcept { return _output; }
private:
    static bool sameSizes(const Voice&, const Voice&) noexcept;
    static void readWindow(const dsp::DelayLine&, dsp::DelayLine&, size_t offset, size_t frames,
                           bool fractionalBeforeWrite = false) noexcept;
    static void commitWrites(const dsp::DelayLine&, dsp::DelayLine&, size_t count) noexcept;
    static void chiffScalars(const dsp::Chiff&, dsp::Chiff&) noexcept;
    static void spatialScalars(const dsp::SpatialSource&, dsp::SpatialSource&) noexcept;
    static void voiceScalars(const Voice&, Voice&) noexcept;
    Voice _shadow;
    const Pipewave* _pipe;
    const bool _allowRebind;
    const Voice* _origin = nullptr;
    juce::Random _pipeRandom, _chiffRandom;
    alignas(64) std::array<float, 16*SUB_FRAME_LENGTH> _output {};
    size_t _chiffWrites = 0, _voiceWrites = 0;
    unsigned _quanta = 1, _overMask = 0;
    bool _captured = false, _rendered = false;
    JUCE_DECLARE_NON_COPYABLE(VoiceSnapshot)
};
}
