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

#pragma once

#include "aeolus/globals.h"
#include "aeolus/list.h"
#include "aeolus/rankwave.h"

#include "aeolus/dsp/delay.h"
#include "aeolus/dsp/chiff.h"
#include "aeolus/dsp/spatial.h"

#include <vector>
#include <atomic>
#include <limits>

AEOLUS_NAMESPACE_BEGIN

class Engine;

/**
 * @brief Single voice associated with a single pipe.
 *
 * Cache-line aligned so that voices rendered on different threads never
 * share a line.
 */
class alignas(64) Voice : public ListItem<Voice>
{
public:
    Voice() = delete;
    Voice(Engine& engine);

    /// Preallocate storage used when triggering (call outside the audio thread).
    void reserveStorage() { _spatialSource.reserveDelay(); }

    void trigger(const Pipewave::State& state);
    void release();
    void reset();
    void process(float* outL, float* outR);
    bool isOver() const noexcept;

    /// True while the pipe is sounding and no release has been requested,
    /// including a release scheduled for later in the current batch.
    bool isActive() const noexcept;
    bool isForNote(int note) const noexcept;
    int getNote() const;

    void setStopIndex(int idx) noexcept { _stopIndex = idx; }
    int stopIndex() const noexcept { return _stopIndex; }

    void resetAndReturnToPool();

    float getPanPosition() const noexcept { return _panPosition; }

    /// Draw pipe instability and chiff noise from this voice's own generators,
    /// seeded deterministically. Otherwise the process-wide shared generators
    /// are used (original behaviour).
    void setRandomSeed(juce::int64 seed);
    void useSharedRandom() noexcept { _ownRandom = false; }

    //------------------------------------------------------
    // Batch rendering (see Engine::renderBatch and docs/MULTICORE.md).

    /// First quantum of the current batch in which the voice sounds.
    void setBatchStart(int quantum) noexcept { _batchStart = quantum; }
    int getBatchStart() const noexcept { return _batchStart; }

    /// Release the voice immediately before rendering the given quantum.
    void scheduleRelease(int quantum) noexcept { _releaseQuantum = quantum; }

    /**
     * Render quanta [getBatchStart(), numQuanta), adding the stereo output
     * of quantum q to outL/outR + q * SUB_FRAME_LENGTH. Stops after the
     * quantum in which the voice is over. Returns the end quantum (exclusive).
     * Safe to call on a worker thread when the voice uses its own random
     * generators; touches no state shared with other voices.
     */
    int renderBatch(float* outL, float* outR, int numQuanta);

    /**
     * Quantum of the current batch after which isOver() becomes true, or
     * numQuanta if that does not happen before numQuanta. Derived from the
     * envelope state and any scheduled release without rendering, mirroring
     * Pipewave::play() and process().
     */
    int predictFinishQuantum(int numQuanta) const noexcept;

    /// Record predictFinishQuantum() for control steps later in the batch.
    void setFinishQuantum(int quantum) noexcept { _finishQuantum = quantum; }

    /**
     * True if the serial renderer would already have removed this voice from
     * its division before the given quantum's control step. The batch
     * renderer reclaims finished voices at the end of the batch instead.
     */
    bool hasFinishedBefore(int quantum) const noexcept { return _finishQuantum < quantum; }

    /// Clear per-batch scheduling.
    void endBatch() noexcept { _batchStart = 0; _releaseQuantum = -1; _finishQuantum = std::numeric_limits<int>::max(); }

private:
    Engine& _engine;
    Pipewave::State _state; ///< Pipe state associated with this voice.

    /// Index of the stop associated with this voice.
    /// This is used to tell which stops are voiced.
    int _stopIndex;

    float _buffer[SUB_FRAME_LENGTH];

    /// Delay after chiff.
    dsp::DelayLine _delayLine;
    int _delay{};

    /// Attack chiff.
    dsp::Chiff _chiff;

    ///  Voice virtual pan position [0..1] used for multibus output
    float _panPosition;

    /// Stereo spatial modeller.
    dsp::SpatialSource _spatialSource;

    /// Counter to account for the delayed sound before recycling the voice.
    size_t _postReleaseCounter;

    bool _ownRandom{ false };
    juce::Random _pipeRandom;
    juce::Random _chiffRandom;

    int _batchStart{ 0 };
    int _releaseQuantum{ -1 };
    int _finishQuantum{ std::numeric_limits<int>::max() };
};

//==============================================================================

/**
 * @brief A collection of all the voices.
 */
class VoicePool final
{
public:
    constexpr static int DefaultMaxVoices = 512;

    VoicePool(Engine& engine, int maxVoices = DefaultMaxVoices);

    Voice* trigger(const Pipewave::State& state);
    void resetAndReturnToPool(Voice* voice);

    int getNumberOfActiveVoices() const noexcept { return _voiceCount; }
    int getMaxVoices() const noexcept { return (int) _voices.size(); }

    /// Seed each triggered voice's own noise generators from a deterministic
    /// sequence instead of using the shared generators.
    void setPerVoiceRandom(bool shouldUsePerVoiceRandom) noexcept { _perVoiceRandom = shouldUsePerVoiceRandom; }
    bool isPerVoiceRandom() const noexcept { return _perVoiceRandom; }

    /// Restart the deterministic seed sequence (audio thread or while stopped).
    void restartRandomSequence() noexcept { _triggerSerial = 0; }

    /// Voices triggered with per-voice noise since the sequence restarted.
    juce::uint64 getTriggerCount() const noexcept { return _triggerSerial; }

private:

    Engine& _engine;

    std::vector<Voice> _voices;     ///< All the voices.
    List<Voice> _idleVoices;        ///< Voices available to be triggered.
    std::atomic<int> _voiceCount;   ///< Number of taken voices.

    bool _perVoiceRandom{ false };
    juce::uint64 _triggerSerial{ 0 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(VoicePool)
};

AEOLUS_NAMESPACE_END
