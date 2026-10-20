// Copyright (C) 2026 Aeolus fork contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "voicesnapshot.h"
#include <algorithm>
#include <cstring>

namespace aeolus {
VoiceSnapshot::VoiceSnapshot(const Voice& preparedVoice, bool allowRebind)
    : _shadow(preparedVoice), _pipe(preparedVoice._state.pipewave), _allowRebind(allowRebind),
      _pipeRandom(0), _chiffRandom(0) {
    if (allowRebind) _shadow._spatialSource._delayLine._buffer.reserve(
        std::max(size_t(SAMPLE_RATE),preparedVoice._spatialSource._delayLine._buffer.capacity()));
}

bool VoiceSnapshot::sameSizes(const Voice& a, const Voice& b) noexcept {
    return a._delayLine.size() && a._chiff._pipeResonator.size()
        && a._spatialSource._delayLine.size()
        && a._delayLine.size() == b._delayLine.size()
        && a._chiff._pipeResonator.size() == b._chiff._pipeResonator.size()
        && a._spatialSource._delayLine.size() == b._spatialSource._delayLine.size();
}
void VoiceSnapshot::readWindow(const dsp::DelayLine& from, dsp::DelayLine& to, size_t offset, size_t frames,
                               bool fractionalBeforeWrite) noexcept {
    const auto size = from.size();
    jassert(size && size == to.size() && offset < size);
    to._writeIndex = from._writeIndex;
    // Nearest reads follow writes: after 'offset' samples, all their input
    // comes from this quantum's own writes. Offset zero needs no history.
    // Chiff reads before writing, with an extra interpolation neighbour.
    const size_t count = fractionalBeforeWrite
        ? std::min(size, std::min(frames, offset+1)+1)
        : std::min(frames, offset);
    if (!count) return;
    size_t end = from._writeIndex+offset;
    if (end >= size) end -= size;
    if (fractionalBeforeWrite) {
        end += 2;
        if (end >= size) end -= size;
        if (end >= size) end -= size;
    }
    const size_t start = end >= count ? end-count : size-(count-end);
    const size_t first = std::min(count, size-start);
    std::memcpy(to._buffer.data()+start, from._buffer.data()+start, first*sizeof(float));
    if (count > first)
        std::memcpy(to._buffer.data(), from._buffer.data(), (count-first)*sizeof(float));
}
void VoiceSnapshot::commitWrites(const dsp::DelayLine& from, dsp::DelayLine& to, size_t count) noexcept {
    jassert(from.size() == to.size());
    count = std::min(count, from.size());
    if (!count) return; // Capture already copied the unchanged cursor.
    const auto start = from._writeIndex;
    const auto first = std::min(count, from.size()-start);
    std::memcpy(to._buffer.data()+start, from._buffer.data()+start, first*sizeof(float));
    if (count > first)
        std::memcpy(to._buffer.data(), from._buffer.data(), (count-first)*sizeof(float));
    to._writeIndex = from._writeIndex;
}
void VoiceSnapshot::chiffScalars(const dsp::Chiff& from, dsp::Chiff& to) noexcept {
    to._noiseEnvelope = from._noiseEnvelope; to._envelope = from._envelope;
    to._envelopeTrigger = from._envelopeTrigger;
    to._pipeDelay = from._pipeDelay; to._pipeRead = from._pipeRead;
    to._lpSpec = from._lpSpec; to._lpState = from._lpState; to._gain = from._gain;
}
void VoiceSnapshot::spatialScalars(const dsp::SpatialSource& from, dsp::SpatialSource& to) noexcept {
    to._sampleRate = from._sampleRate;
    to._sourcePosition = from._sourcePosition; to._listenerPosition = from._listenerPosition;
    to._listenerOrientation = from._listenerOrientation;
    to._listenerLeftRightDistance = from._listenerLeftRightDistance;
    to._leftDelay = from._leftDelay; to._rightDelay = from._rightDelay;
    to._leftAttenuation = from._leftAttenuation; to._rightAttenuation = from._rightAttenuation;
    std::copy(std::begin(from._filterSpec), std::end(from._filterSpec), std::begin(to._filterSpec));
    std::copy(std::begin(from._filterState), std::end(from._filterState), std::begin(to._filterState));
}
void VoiceSnapshot::voiceScalars(const Voice& from, Voice& to) noexcept {
    to._state = from._state; to._stopIndex = from._stopIndex;
    to._delay = from._delay; to._panPosition = from._panPosition;
    to._postReleaseCounter = from._postReleaseCounter;
    // prepareSignal() clears the scratch buffer before every use.
    // Intrusive list links and the engine reference always stay with their owner.
}
bool VoiceSnapshot::capture(const Voice& from, const juce::Random& pipeRandom,
                            const juce::Random& chiffRandom, unsigned quanta) noexcept {
    _captured = _rendered = false;
    if (_allowRebind) {
        if (!from._state.playbackLease) return false;
        _pipe = from._state.playbackLease.get();
        auto& buffer=_shadow._spatialSource._delayLine._buffer;
        const auto size=from._spatialSource._delayLine.size();
        if (size>buffer.capacity()) return false;
        if (buffer.size()!=size) buffer.resize(size);
    }
    if (!quanta || quanta > 8 || !_pipe || from._state.pipewave != _pipe || from.isOver()
        || _pipe->doesNeedToBeRebuilt() || !sameSizes(from, _shadow)) return false;
    _origin = &from;
    _quanta = quanta;
    const size_t frames = quanta*SUB_FRAME_LENGTH;
    voiceScalars(from, _shadow);
    const auto& chiff = from._chiff;
    chiffScalars(chiff, _shadow._chiff);
    spatialScalars(from._spatialSource, _shadow._spatialSource);
    readWindow(from._delayLine, _shadow._delayLine, from._delayLine.prepareNearestRead(from._delay), frames);
    const auto& spatial = from._spatialSource;
    readWindow(spatial._delayLine, _shadow._spatialSource._delayLine,
               spatial._delayLine.prepareNearestRead(spatial._leftDelay), frames);
    readWindow(spatial._delayLine, _shadow._spatialSource._delayLine,
               spatial._delayLine.prepareNearestRead(spatial._rightDelay), frames);
    const bool quiet = chiff._noiseEnvelope.state() == dsp::Envelope::Sustain
        && chiff._envelope.state() == dsp::Envelope::Sustain
        && chiff._envelope.level()*chiff._gain < 1e-4f;
    _chiffWrites = chiff.isActive() && !quiet ? SUB_FRAME_LENGTH : 0;
    if (_chiffWrites)
        readWindow(chiff._pipeResonator, _shadow._chiff._pipeResonator, chiff._pipeRead.offset, frames, true);
    else
        _shadow._chiff._pipeResonator._writeIndex = chiff._pipeResonator._writeIndex;
    _pipeRandom = pipeRandom; _chiffRandom = chiffRandom;
    _captured = true;
    return true;
}
void VoiceSnapshot::render() noexcept {
    jassert(_captured && !_rendered);
    std::fill_n(_output.begin(), _quanta*2*SUB_FRAME_LENGTH, 0.0f);
    _voiceWrites = _chiffWrites = _overMask = 0;
    for (unsigned q=0; q<_quanta; ++q) {
        if (_shadow.isOver()) { _overMask |= 1u<<q; continue; }
        const auto& chiff = _shadow._chiff;
        const bool quiet = chiff._noiseEnvelope.state() == dsp::Envelope::Sustain
            && chiff._envelope.state() == dsp::Envelope::Sustain
            && chiff._envelope.level()*chiff._gain < 1e-4f;
        if (chiff.isActive() && !quiet) _chiffWrites += SUB_FRAME_LENGTH;
        _shadow.process(_output.data()+q*2*SUB_FRAME_LENGTH,
                        _output.data()+(q*2+1)*SUB_FRAME_LENGTH, _pipeRandom, _chiffRandom);
        _voiceWrites += SUB_FRAME_LENGTH;
        if (_shadow.isOver()) _overMask |= 1u<<q;
    }
    _rendered = true;
}
void VoiceSnapshot::adopt(Voice& to, juce::Random& pipeRandom, juce::Random& chiffRandom) const noexcept {
    jassert(_captured && _rendered && _origin == &to && sameSizes(_shadow, to));
    commitWrites(_shadow._delayLine, to._delayLine, _voiceWrites);
    commitWrites(_shadow._chiff._pipeResonator, to._chiff._pipeResonator, _chiffWrites);
    commitWrites(_shadow._spatialSource._delayLine, to._spatialSource._delayLine, _voiceWrites);
    voiceScalars(_shadow, to);
    chiffScalars(_shadow._chiff, to._chiff);
    spatialScalars(_shadow._spatialSource, to._spatialSource);
    pipeRandom = _pipeRandom; chiffRandom = _chiffRandom;
}
}
