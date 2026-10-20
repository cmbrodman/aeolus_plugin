// ----------------------------------------------------------------------------
//
//  Copyright (C) 2021 Arthur Benilov <arthur.benilov@gmail.com>
//  Copyright (C) 2003-2013 Fons Adriaensen <fons@linuxaudio.org>
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
#include <mutex>
#include "aeolus/registration.h"

#include "aeolus/globals.h"
#include "aeolus/addsynth.h"
#include "aeolus/scale.h"

#include <vector>

AEOLUS_NAMESPACE_BEGIN

/**
 * @brief Single pipe wavetable.
 *
 * This class represents a single pipe mapped to a model (additive synth),
 * note, and frequency.
 */
class Pipewave final
{
public:

    /// Envelope state.
    enum EnvState
    {
        Idle,
        Attack,
        Release,
        Over
    };

    // A playback lease only changes an atomic counter. Storage is reclaimed by
    // Rankwave on the setup thread, never by the last audio-thread release.
    class Lease {
    public:
        Lease() = default;
        explicit Lease(Pipewave*);
        Lease(const Lease&);
        Lease& operator=(const Lease&);
        ~Lease();
    private:
        Pipewave* _pipe = nullptr;
    };
    bool hasPlaybackLeases() const noexcept { return _playbackLeases.load() != 0; }
#if AEOLUS_TESTING
    inline static std::atomic<unsigned> testWaveformBuildCount{0};
    inline static std::atomic<unsigned> testLivePipeCount{0};
#endif

    /// Playback state.
    struct State
    {
        Lease lease;
        Pipewave *pipewave = nullptr;
        EnvState env = Idle;
        float* playPtr = nullptr;               // _p_p
        float playInterpolation = 0.0;          // _y_p
        float playInterpolationSpeed = 0.0f;    // _z_p
        float* releasePtr = nullptr;            // _p_r
        float releaseInterpolation = 0.0f;      // _y_r
        float releaseGain = 0.0f;               // _g_r
        int releaseCount = 0;                   // _i_r

        float gain = 1.0f;
        float chiffGain = 0.0f;

        void release() { if (pipewave != nullptr) pipewave->release(*this); }
        bool isTriggered() const noexcept { return pipewave != nullptr && env == Attack; }
        bool isIdle() const noexcept { return env == Idle; }
        bool isOver() const noexcept { return env == Over; }
        void reset() { lease = Lease{}; pipewave = nullptr; env = Idle; }
    };

    Pipewave() = delete;
    Pipewave(Addsynth& model, int note, float freq, std::shared_ptr<Addsynth> modelOwner = {});
    Pipewave(Pipewave& other);
    ~Pipewave();

    const Addsynth& getModel() const noexcept { return _model; }

    // After changing the frequency of the pipe, the wavetable must be regenerated
    // by calling prepareToPlay() method.
    void setFrequency(float f) noexcept { _freq = f; }
    void setNeedsToBeRebuilt(bool v) noexcept { _needsToBeRebuilt = v; }
    bool doesNeedToBeRebuilt() const noexcept { return _needsToBeRebuilt.load(); }

    int getNote() const noexcept { return _note + _model.getNoteMin(); }

    /// Number of quanta played in the Release state before the pipe is Over.
    int getReleaseQuanta() const noexcept { return _releaseLength; }
    float getFreqency() const noexcept { return _freq; }
    float getPipeFrequency() const noexcept;

    void prepateToPlay(float sampleRate);

    State trigger();
    void release(Pipewave::State& state);

    /// Render one quantum, drawing pipe instability from the process-wide
    /// shared generator (original behaviour).
    void play(State& state, float* out);

    /// Render one quantum, drawing pipe instability from the supplied
    /// generator, which must not be used concurrently by another thread.
    void play(State& state, float* out, juce::Random& rnd);

private:
    void genwave();

    static void looplen(float f, float sampleRate, int lmax, int& aa, int& bb);
    static void attgain(float* att, int n, float p);

    std::atomic<unsigned> _playbackLeases { 0 };
    std::shared_ptr<Addsynth> _modelOwner; // private reload model; released only with the pipe off audio
    Addsynth& _model;
    int _note;
    float _freq;
    float _sampleRate;

    // Tells whether this pipewave needs to be re-generated.
    // This is required for example when changing the tuninig.
    std::atomic<bool> _needsToBeRebuilt;

    int _attackLength;          // _l0
    int _loopLength;            // _l1
    int _sampleStep;            // _k_s
    int _releaseLength;         // _k_r
    float _releaseMultiplier;   // _m_r
    float _releaseDetune;       // _d_r
    float _instability;         // _d_p

    std::vector<float> _wavetable;

    float* _attackStartPtr; // _p0
    float* _loopStartPtr;   // _p1
    float* _loopEndPtr;     // _p2
};

//==============================================================================

/**
 * @brief Pipes across the keys range.
 *
 * This class is a collection of pipes based on the same
 * additive synth model.
 */
class Rankwave
{
public:
    Rankwave(Addsynth& model);

    void createPipes(const Scale& scale, float tuningFreq, std::shared_ptr<Addsynth> modelOwner = {});
    void reserveModelReplacement(); // setup, before callback ownership
    void adoptPrepared(Rankwave& staged); // moves a fully prepared private generation


    // Recalculate pipes tuning based on the current global scale and A4 frequency,
    // or global MTS tuning if enabed.
    void retunePipes(const Scale& scale, float tuningFreq, std::shared_ptr<Addsynth> modelOwner = {});

    juce::String getStopName() const { return _model.getStopName(); }
    bool isForNote(int note) const noexcept { return note >= _noteMin && note <= _noteMax; }
    int getNoteMin() const noexcept { return _noteMin; }
    int getNoteMax() const noexcept { return _noteMax; }

    void prepareToPlay(float sampleRate);
    void collectRetired();

    Pipewave::State trigger(int note);

    void recreateFromModel(const Scale& scale, float tuningFreq)
{
    const std::lock_guard<std::recursive_mutex> lock(_preparationMutex);
    _noteMin = _model.getNoteMin();
    _noteMax = _model.getNoteMax();
    createPipes(scale, tuningFreq);
}

private:
    Addsynth& _model;
    PublishedValue<int> _noteMin;
    PublishedValue<int> _noteMax;

    std::recursive_mutex _preparationMutex; // setup/collector threads only
    struct Generation { std::vector<std::shared_ptr<Pipewave>> pipes; int firstNote = 0; };
    std::unique_ptr<Generation> _prepared;
    std::unique_ptr<Generation> _ownedCurrent;
    std::vector<std::unique_ptr<Generation>> _retired;
    std::atomic<Generation*> _current { nullptr };
    // Protects the interval between loading the published generation and
    // acquiring a State lease. Sequential consistency pairs with publication
    // and reclamation; it is not a spinlock and the callback never waits.
    std::atomic<unsigned> _triggerReaders { 0 };
    float _preparedSampleRate = 0;

};

AEOLUS_NAMESPACE_END
