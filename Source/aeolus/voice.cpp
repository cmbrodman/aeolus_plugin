// ----------------------------------------------------------------------------
//
//  Copyright (C) 2021 Arthur Benilov <arthur.benilov@gmail.com>
//
//  This program is free software; you can redistribute it and/or modify
//  it under the terms of the GNU General Public License as published by
//  the Free Software Foundation; either version 3 of the License, or
//  (at your option) any later version.
//
//  This program is distributed in the hope that it will be useful,
//  but WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//  GNU General Public License for more details.
//
//  You should have received a copy of the GNU General Public License
//  along with this program.  If not, see <http://www.gnu.org/licenses/>.
//
// ----------------------------------------------------------------------------

#include "aeolus/voice.h"
#include "aeolus/engine.h"

using namespace juce;

AEOLUS_NAMESPACE_BEGIN

Voice::Voice(Engine& engine)
    : _engine(engine)
    , _state{}
    , _stopIndex{-1}
    , _buffer{0}
    , _delayLine{SAMPLE_RATE}
    , _delay{0}
    , _chiff{}
    , _panPosition{0.0f}
    , _spatialSource{}
{
}

void Voice::trigger(const Pipewave::State& state)
{
    jassert(_state.isIdle());
    _state = state;

    // Chiff
    const auto freq = _state.pipewave->getPipeFrequency();
    const auto dt = 1.0f / freq;

    // Delay pipe harmonic signal so that chiff noise builds up first
    _delay = (int) jmin((float)_delayLine.size(), 0.5f * dt * SAMPLE_RATE_F);

    _chiff.setAttack(5.0f * dt);
    _chiff.setDecay(100.0f * dt);
    _chiff.setSustain(0.01f);
    _chiff.setRelease(100.0f * dt);

    // Frequency-dependant chiff attenuation
    float att = 1.0f - expf(-freq / 3000.0f);
    _chiff.setGain(jmin(1.0f, 0.02f * _state.chiffGain * att));
    _chiff.setFrequency(freq);
    _chiff.trigger();

    // Spatialisation
    int note = _state.pipewave->getNote();
    float k = note % 2 != 0 ? 1.0f : -1.0f;

    // Wider spread for low-pitched pipes
    const auto& model = _state.pipewave->getModel();
    const float width = 0.15f * model.getFd() / model.getFn();

    float x = width * k * (float)abs(note - 65);

    // Assuming notes range [36..96]
    float n = k * float(abs(note - 65)); // ~[-30..30]
    _panPosition = jlimit(0.0f, 1.0f, (n + 30.0f) / 60.0f);

    _spatialSource.setSampleRate(SAMPLE_RATE_F);
    _spatialSource.setSourcePosition(x, 5.0f);
    _spatialSource.recalculate();
    _postReleaseCounter = _spatialSource.getPostFxSamplesCount() + 2 * _delay + (int)TREMULANT_DELAY_LENGTH;

    // The pipe delay is only read _delay samples behind the newest write, so
    // clearing that prefix replaces the full clear previously done when the
    // voice was returned to the pool (see DelayLine::resetForReadIndex).
    if (!dsp::useFullVoiceClear())
        _delayLine.resetForReadIndex(_delay);
}

void Voice::release()
{
    // This voice is pending to be reclaimed.
    if (_state.env == Pipewave::Over)
        return;

    _state.release();

    _chiff.release();
}

void Voice::reset()
{
    _state.reset();
    _stopIndex = -1;

    memset(_buffer, 0, sizeof(float) * SUB_FRAME_LENGTH);

    // Otherwise the next trigger() clears the readable part of the pipe delay
    // and chiff resonator and resets the chiff filter, rather than clearing
    // 2 x 44,100 floats here for every finished voice.
    if (dsp::useFullVoiceClear()) {
        _delayLine.reset();
        _chiff.reset();
    }
    _spatialSource.reset();

    endBatch();
}

void Voice::setRandomSeed(juce::int64 seed)
{
    _ownRandom = true;
    _pipeRandom.setSeed(seed);
    _chiffRandom.setSeed(seed ^ (juce::int64) 0x5851f42d4c957f2dLL);
}

void Voice::process(float* outL, float* outR)
{
    if (_engine.isEmergencyMuted()) {
        juce::FloatVectorOperations::clear(outL, SUB_FRAME_LENGTH);
        juce::FloatVectorOperations::clear(outR, SUB_FRAME_LENGTH);
        return;
    }
    memset(_buffer, 0, sizeof(float) * SUB_FRAME_LENGTH);

    const auto gain = _state.gain;

    if (_state.env == Pipewave::Over) {
        _postReleaseCounter -= std::min((int)_postReleaseCounter, SUB_FRAME_LENGTH);

        for (int i = 0; i < SUB_FRAME_LENGTH; ++i) {
            _delayLine.write(0.0f);
            _buffer[i] = _delayLine.readNearest(_delay) * gain;
        }

    } else {
        auto* pipe = _state.pipewave;

        if (_ownRandom)
            pipe->play(_state, _buffer, _pipeRandom);
        else
            pipe->play(_state, _buffer);

        for (int i = 0; i < SUB_FRAME_LENGTH; ++i) {
            _delayLine.write(_buffer[i]);
            _buffer[i] = _delayLine.readNearest(_delay) * gain;
        }
    }

    if (_ownRandom)
        _chiff.process(_buffer, SUB_FRAME_LENGTH, _chiffRandom);
    else
        _chiff.process(_buffer, SUB_FRAME_LENGTH);

    // Spatial modellig is only applied on stereo voice output
    if (outL != outR) {
        _spatialSource.process(_buffer, outL, outR, SUB_FRAME_LENGTH);
    } else {
        memcpy(outL, _buffer, sizeof(float) * SUB_FRAME_LENGTH);
    }
}

bool Voice::isOver() const noexcept
{
    return (_state.env == Pipewave::Over) && _postReleaseCounter == 0;
}

bool Voice::isActive() const noexcept
{
    return _state.env == Pipewave::Attack && _releaseQuantum < 0;
}

int Voice::predictFinishQuantum(int numQuanta) const noexcept
{
    auto env = _state.env;
    const auto* pipe = _state.pipewave;

    // Attack without a scheduled release cannot end, unless the pipe has been
    // retuned (play() then stops the voice immediately).
    const bool rebuild = pipe != nullptr && pipe->doesNeedToBeRebuilt();

    if (env == Pipewave::Attack && _releaseQuantum < 0 && !rebuild)
        return numQuanta;

    bool started = _state.playPtr != nullptr || _state.releasePtr != nullptr;
    bool releaseStarted = _state.releasePtr != nullptr;
    int releaseCount = _state.releaseCount;
    size_t counter = _postReleaseCounter;

    for (int q = _batchStart; q < numQuanta; ++q) {
        // Voice::release() -> Pipewave::release()
        if (q == _releaseQuantum && env != Pipewave::Over && pipe != nullptr)
            env = started ? Pipewave::Release : Pipewave::Over;

        // Voice::process()
        if (env == Pipewave::Over) {
            counter -= std::min(counter, (size_t) SUB_FRAME_LENGTH);

            if (counter == 0)
                return q;

            continue;
        }

        // Pipewave::play()
        if (rebuild) {
            env = Pipewave::Over;
            continue;
        }

        if (env == Pipewave::Attack) {
            started = true;
        } else if (env == Pipewave::Release) {
            if (!releaseStarted) {
                releaseStarted = true;
                releaseCount = pipe->getReleaseQuanta();
            }

            const int i = releaseCount - 1;

            if (i > 0)
                releaseCount = i;
            else
                env = Pipewave::Over;
        }
    }

    return numQuanta;
}

int Voice::renderBatch(float* outL, float* outR, int numQuanta)
{
    float left[SUB_FRAME_LENGTH];
    float right[SUB_FRAME_LENGTH];

    for (int q = _batchStart; q < numQuanta; ++q) {
        if (_engine.isEmergencyMuted()) return q;
        // Same order as the serial renderer: the division's control step
        // releases the voice before the quantum is rendered.
        if (q == _releaseQuantum)
            release();

        process(left, right);

        float* const l = outL + q * SUB_FRAME_LENGTH;
        float* const r = outR + q * SUB_FRAME_LENGTH;

        for (int i = 0; i < SUB_FRAME_LENGTH; ++i) {
            l[i] += left[i];
            r[i] += right[i];
        }

        if (isOver())
            return q + 1;
    }

    return numQuanta;
}

bool Voice::isForNote(int note) const noexcept
{
    if (_state.pipewave != nullptr)
        return _state.pipewave->getNote() == note;

    return false;
}

int Voice::getNote() const
{
    if (_state.pipewave != nullptr)
        return _state.pipewave->getNote();

    return -1;
}

void Voice::resetAndReturnToPool()
{
    _engine.getVoicePool().resetAndReturnToPool(this);
}

//==============================================================================

VoicePool::VoicePool(Engine& engine, int maxVoices)
    : _engine{engine}
    , _voices(maxVoices, engine)
    , _idleVoices{}
    , _voiceCount{0}
{
    for (auto& voice : _voices) {
        // The voices are copies of one prototype; copying does not keep
        // reserved vector capacity, so reserve on every voice.
        voice.reserveStorage();
        _idleVoices.append(&voice);
    }
}

Voice* VoicePool::trigger(const Pipewave::State& state)
{
    if (auto* voice = _idleVoices.first()) {
        _idleVoices.remove(voice);
        voice->endBatch();

        if (_perVoiceRandom) {
            // SplitMix64 of the trigger sequence number: deterministic for a
            // given note/registration sequence, independent of thread count.
            juce::uint64 z = (++_triggerSerial) * 0x9e3779b97f4a7c15ULL;
            z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
            z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
            voice->setRandomSeed((juce::int64) (z ^ (z >> 31)));
        } else {
            voice->useSharedRandom();
        }

        voice->trigger(state);
        ++_voiceCount;

        return voice;
    }

    // No more voices.
    return nullptr;
}

void VoicePool::resetAndReturnToPool(Voice* voice)
{
    jassert(voice != nullptr);

    voice->reset();
    _idleVoices.append(voice);
    --_voiceCount;
}

AEOLUS_NAMESPACE_END
