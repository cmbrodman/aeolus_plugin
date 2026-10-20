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
#include "aeolus/ringbuffer.h"
#include "aeolus/scale.h"
#include "aeolus/voice.h"
#include "aeolus/addsynth.h"
#include "aeolus/rankwave.h"
#include "aeolus/division.h"
#include "aeolus/sequencer.h"
#include "aeolus/audioparam.h"
#include "aeolus/levelmeter.h"
#include "aeolus/dsp/reverbswitch.h"
#include <mutex>
#include "aeolus/dsp/interpolator.h"
#include "aeolus/dsp/limiter.h"
#include "aeolus/rtconfig.h"
#include "aeolus/dspteam.h"
#include "mts/libMTSClient.h"
#include <array>
#include <optional>
#include <vector>
#include <map>

AEOLUS_NAMESPACE_BEGIN

class Engine;
class PanicControl;

/**
 * @brief A global shared instance of the organ engine.
 *
 * This class in a singleton which is shared among all the plugin instances.
 */
class EngineGlobal : public juce::DeletedAtShutdown,
                     private juce::Timer
{
public:

    class ProcessorProxy
    {
    public:
        virtual ~ProcessorProxy() = default;
        virtual juce::AudioProcessor* getAudioProcessor() = 0;
        virtual Engine& getEngine() = 0;
        virtual void killAllVoices() = 0;
        virtual int getNumberOfActiveVoices() = 0;
    };

    void registerProcessorProxy(ProcessorProxy* proxy);
    void unregisterProcessorProxy(ProcessorProxy* proxy);

    class Listener
    {
    public:
        virtual ~Listener() = default;
        virtual void onUIScalingFactorChanged(float scalingPercent) = 0;
    };

    void addListener(Listener* listener);
    void removeListener(Listener* listener);

    /**
     * Impulse response descriptor for IRs embedded as binary resources.
     */
    struct IR
    {
        juce::String name;
        const char* data;
        size_t size;

        float gain;
        bool zeroDelay;
        size_t startOffset; // Sample offset from the beginning of the IR waveform

        juce::AudioBuffer<float> waveform;
    };

    void loadSettings();
    void saveSettings(bool reportErrors = true);

    // Zero selects the startup pool size. Logical limits never resize the pool.
    int getVoiceLimitSetting() const noexcept { return _voiceLimit.load(); }
    void setVoiceLimitSetting(int limit) noexcept { _voiceLimit.store(juce::jlimit(0, 4096, limit)); }
    int getMeterWarningPercent() const noexcept { return _meterWarning.load(); }
    void setMeterWarningPercent(int percent) noexcept { _meterWarning.store(juce::jlimit(50, 89, percent)); }
    int getMeterCriticalPercent() const noexcept { return _meterCritical.load(); }
    void setMeterCriticalPercent(int percent) noexcept { _meterCritical.store(juce::jlimit(90, 100, percent)); }

    int getStopsCount() const noexcept { return _rankwaves.size(); }
    Rankwave* getStop(int i) { return _rankwaves[i]; }

    juce::StringArray getAllStopNames() const;
    Rankwave* getStopByName(const juce::String& name);

    const std::vector<IR>& getIRs() const noexcept { return _irs; }
    int getLongestIRLength() const noexcept { return _longestIRLength; }

    void updateStops(float sampleRate);

    float getTuningFrequency() const noexcept { return _tuningFrequency; }
    void setTuningFrequency(float f) noexcept { _tuningFrequency = f; }

    const Scale& getScale() const noexcept { return _scale; }
    void setScaleType(Scale::Type type) noexcept { _scale.setType(type); }

    bool isConnectedToMTSMaster();
    juce::String getMTSScaleName();
    float getMTSNoteToFrequency(int midiNote, int midiChannel = -1);
    bool shouldMTSFilterNote(int midiNote, int midiChannel = -1);
#if AEOLUS_TESTING
    void setMTSFrequencyBaseForTest(float base) { _testMTSFrequencyBase.store(base); }
#endif

    bool isMTSEnabled() const { return _mtsEnabled; }
    void setMTSEnabled(bool shouldBeEnabled);

    float getUIScalingFactor() const noexcept { return _uiScalingFactor; }
    void setUIScalingFactor(float f);

    bool isKeyboardVisible() const noexcept { return _keyboardVisible; }
        void setKeyboardVisible(bool v) noexcept { _keyboardVisible = v; }

    void rebuildRankwaves();

    bool reloadStopFile(const juce::File& file); // true when the model is ready, false if deferred/failed
    bool hasDeferredStopReloads() const noexcept { return !_deferredStopFiles.isEmpty(); }

    JUCE_DECLARE_SINGLETON (EngineGlobal, false)

private:
    EngineGlobal();
    ~EngineGlobal() override;

    void loadRankwaves();
    void loadIRs();

    /**
     * Refresh MTS tuning table for all MIDI notes.
     * Returns true if there was a change to the tuning.
     */
    bool updateMTSTuningCache();

    // juce::Timer
    void timerCallback() override;

    juce::Array<ProcessorProxy*> _processors;

    juce::ListenerList<Listener> _listeners;

    std::mutex _rankwavesMutex; // setup-only registry publication vs device preparation
    juce::OwnedArray<Rankwave> _rankwaves;
    juce::HashMap<juce::String, Rankwave*> _rankwavesByName;

    std::vector<IR> _irs;
    int _longestIRLength;   ///< Longest IR length in samples

    std::atomic<float> _sampleRate{SAMPLE_RATE_F};
    Scale _scale;
    float _tuningFrequency;

    void injectKeepAlive(float* outL, float* outR, int numFrames);  // Keeps audio stream alive

        int _samplesSinceAudio { 0 };
        int _keepAliveSamplesLeft { 0 };
        bool _streamNeedsPrime { true };
        uint32_t _keepAliveRng { 1 };

    MTSClient* _mtsClient{};
    bool _mtsEnabled{};
    std::array<float, 128> _mtsTuningCache{};
#if AEOLUS_TESTING
    std::atomic<float> _testMTSFrequencyBase{0};
#endif

    float _uiScalingFactor{ UI_SCALING_DEFAULT };

    bool _keyboardVisible { true };
    std::atomic<int> _voiceLimit { 0 };
    std::atomic<int> _meterWarning { 75 }, _meterCritical { 90 };

    juce::ApplicationProperties _globalProperties;

    void pollStopFiles();
        juce::HashMap<juce::String, juce::int64> _stopFileModTimes;
        int _stopPollCounter { 0 };
    juce::StringArray _deferredStopFiles;

};

//==============================================================================

/**
 * @brief Organ engine.
 * This class defines the top-level organ engine that performs MIDI events processing
 * and audio generation.
 */
class Engine : private juce::Timer
{
    // First declared, last destroyed: retain the guardian through all DSP/member
    // teardown, but stop its receiver before any Engine fields disappear.
    std::unique_ptr<PanicControl> _panicControl;

public:

    //--------------------------------------------------------------------------

    struct NoteEvent
    {
        bool on;
        int note;
        int midiChannel;
    };

    struct Level
    {
        LevelMeter left;
        LevelMeter right;
    };

    enum {
        VOLUME = 0,

        NUM_PARAMS
    };

    //--------------------------------------------------------------------------

    Engine();
    ~Engine() override;

    bool isPedalDivision(const Division* d) const;
        Division* getPedalDivision();
        void applyCouplerLayout(int sourceIndex, bool hasBassCoupler,
                            const std::vector<Division::LinkSpec>& specs);

    struct CouplerEdits
    {
        int sourceIndex = 0;
        bool hasBassCoupler = false;
        std::vector<Division::LinkSpec> specs;
    };
    void applyCouplerLayouts(const juce::Array<CouplerEdits>& edits);

    /**
     * This method returns external processing sample rate as mandated
     * by the plugin host. Internally the organ engine performs processing
     * with a fixed SAMPLE_RATE.
     */
    float getSampleRate() const noexcept { return _sampleRate; }

    /**
     * Returns the number of active (playing) voices.
     */
    int getVoiceCount() const noexcept { return _voicePool.getNumberOfActiveVoices(); }

    /**
     * Called by the host pefore starting requesting the audio blocks.
     */
    void prepareToPlay(float sampleRate, int frameSize);

    /**
     * Set the reverb IR bu its number.
     * @note Requests preparation off the audio thread.
     */
    void setReverbIR(int num);

    /**
     * Set the reverb IR by its number asynchronously.
     * This to be called on the main (UI) thread.
     */
    void postReverbIR(int num);

    /**
     * Returns currently set reverb IR number.
     */
    int getReverbIR() const noexcept { return _selectedIR; }

    /**
     * Returns the reverb tail in seconds.
     */
    float getReverbLengthInSeconds() const;

    /**
     * Set reverb wet output level (linear).
     * @note This must be called on the audio thread.
     */
    void setReverbWet(float v);

    /**
     * Set global output volume level (linear).
     * @note This must be called on the audio thread.
     */
    void setVolume(float v);

    /**
     * Returns volume levels.
     */
    Level& getVolumeLevel() noexcept { return _volumeLevel; }

    /**
     * Enable or disable master limiter.
     */
    void enableLimiter(bool shouldBeEnabled);

    /**
     * @return Whether the limiter is enabled.
     */
    bool isLimiterEnabled() const;

    /**
     * Returns currently set MIDI control channel.
     */
    int getMIDIControlChannelsMask() const noexcept { return _midiControlChannelsMask; }

    /**
     * Assign MIDI channel to be used to control the organ stops and sequencer.
     */
    void setMIDIControlChannelsMask(int mask);
    /**
    * Returns currently set MIDI control channel.
    */
    int getMIDISwellChannelsMask() const noexcept { return _midiSwellChannelsMask; }

    /**
    * Assign MIDI channel to be used to control the organ stops and sequencer.
    */
    void setMIDISwellChannelsMask(int mask);

    /**
     * Generate audio.
     */
    void process(float* outL, float* outR, int numFrames, bool isNonRealtime = false);

    // Multibus version of the processing (does not include the convolver).
    void process(juce::AudioBuffer<float>& out, bool isNonRealtime = false,
                 int startSample = 0, int numSamples = -1);

    /**
     * Process incoming MIDI messages.
     */
    void processMIDIMessage(const juce::MidiMessage& message);

    /**
     * Handle note-on events.
     */
    void noteOn(int note, int midiChannel);

    /**
     * Handle note-off events.
     */
    void noteOff(int note, int midiChannel);

    /**
     * Release all the active voices immediately.
     */
    void allNotesOff();

    // Audio callback publishes progress even when optional profiling is disabled.
    static_assert(std::atomic<uint64_t>::is_always_lock_free, "Audio heartbeat must be lock-free");
    void audioCallbackStarted() noexcept { _audioStarted.fetch_add(1, std::memory_order_relaxed); }
    void audioCallbackCompleted() noexcept { _audioCompleted.fetch_add(1, std::memory_order_release); }
    uint64_t getAudioCompleted() const noexcept { return _audioCompleted.load(std::memory_order_acquire); }
    uint64_t getAudioStarted() const noexcept { return _audioStarted.load(std::memory_order_acquire); }
    uint64_t getGuiProgress() const noexcept { return _guiProgress.load(std::memory_order_acquire); }
    bool isAudioActive() const noexcept { return getAudioState() == 1; }
    unsigned getAudioState() const noexcept { return _audioState.load(std::memory_order_acquire); }
    void restoreAudioState(unsigned state) noexcept { _audioState.store(state,std::memory_order_release); }
    void setAudioPreparing() noexcept { _audioState.store(2, std::memory_order_release); }
    void setAudioActive(bool active) noexcept { _audioState.store(active ? 1u : 0u, std::memory_order_release); }

    // Requests may come from the UI or the independent control thread.
    unsigned requestEmergencyPanic() noexcept {
        _panicMuted.store(true, std::memory_order_release);
        return _panicRequested.fetch_add(1, std::memory_order_acq_rel) + 1;
    }
    bool hasSafetyWatchdog() const;
    void beginShutdown();
    bool safetyWatchdogStarting() const;
    void cancelEmergencyResume() noexcept { _resumeAuthorized.store(~0u,std::memory_order_release); }
    bool authorizeEmergencyResume(unsigned epoch) noexcept {
        if(epoch!=getPanicRequested() || epoch!=getPanicAcknowledged()) return false;
        _resumeAuthorized.store(epoch,std::memory_order_release);
        return true;
    }
    bool isEmergencyPanicPending() const noexcept { return _panicRequested.load(std::memory_order_acquire) != _panicAcknowledged.load(std::memory_order_acquire); }
    bool isEmergencyMuted() const noexcept { return _panicMuted.load(std::memory_order_acquire) || isEmergencyPanicPending(); }
    unsigned getPanicRequested() const noexcept { return _panicRequested.load(std::memory_order_acquire); }
    unsigned getPanicAcknowledged() const noexcept { return _panicAcknowledged.load(std::memory_order_acquire); }
#if AEOLUS_TESTING
    void setPanicTestDelay(unsigned delay) noexcept { _panicTestDelay.store(delay); _panicTestStarts.store(0); }
    unsigned getPanicTestStarts() const noexcept { return _panicTestStarts.load(std::memory_order_acquire); }
#endif


    juce::MidiKeyboardState& getMidiKeyboardState() noexcept { return _midiKeyboardState; }

    juce::Range<int> getMidiKeyboardRange() const;

    std::set<int> getKeySwitches() const;

    VoicePool& getVoicePool() noexcept { return _voicePool; }
    int getVoiceLimit() const noexcept;
    uint64_t getRejectedNoteCount() const noexcept { return _rejectedNotes.load(); }
    uint64_t getRejectedRegistrationCount() const noexcept { return _rejectedRegistrations.load(); }
    void resetAdmissionCounters() noexcept { _rejectedNotes.store(0); _rejectedRegistrations.store(0); }
    unsigned getLayoutEpoch() const noexcept { return _layoutEpoch.load(); }
    bool postUIControl(ControlAction action, int division = -1, int index = 0, int value = 0);
    void applyPendingControlsForSetup() { processControlEvents(1023); } // processing must be suspended
    void setOwningProcessor(juce::AudioProcessor* processor) { _owningProcessor = processor; }
#if AEOLUS_TESTING
    juce::String getPendingErrorForTest() const { return _lastError; }
    size_t getPendingControlCount() const { return _controlEvents.count(); }
#endif
    void requestProgram(int index) noexcept;
    uint64_t getRejectedControlCount() const noexcept { return _rejectedControls.load(); }
    void registrationChanged() noexcept { _registrationRevision.fetch_add(1, std::memory_order_relaxed); }

    /// Scheduling/rendering configuration read from the environment.
    const RealtimeConfig& getRealtimeConfig() const noexcept { return _rtConfig; }

    /// Batch renderer worker team, or nullptr when the serial renderer is used.
    DspTeam* getDspTeam() noexcept { return _dspTeam.get(); }

    /// Rendered quanta that were not consumed by the process() call that
    /// rendered them. Always zero unless the batch length prediction is wrong.
    juce::uint64 getBatchMispredictions() const noexcept { return _batchMispredictions; }

    int getDivisionCount() const noexcept { return _divisions.size(); }
    Division* getDivisionByIndex(int i) { return _divisions[i]; }
    Division* getDivisionByName(const juce::String& name);

    Sequencer* getSequencer() noexcept { return _sequencer.get(); }

    juce::var getPersistentState() const;
    void setPersistentState(const juce::var& state);
    bool canRestoreState(const juce::var& state) const;

    void loadOrganState();
    void saveOrganState() const;
    void requestSaveOrganState();
    
    void applyDivisionLayout(int count, const juce::StringArray& names);

    bool applyDivisionStops(int divisionIndex, const juce::StringArray& pipeNames, bool reload = true, juce::var* pendingConfig = nullptr);

    struct DivisionStopEdits
        {
            int divisionIndex = 0;
            juce::StringArray pipeNames;
        };
        bool applyDivisionStopsList(const juce::Array<DivisionStopEdits>& edits);

    void setWindowSize(int width, int height);
        int getWindowWidth() const noexcept { return _windowWidth; }
        int getWindowHeight() const noexcept { return _windowHeight; }

    void postNoteEvent(bool onOff, int note, int midiChannel);

private:
    struct ControlEvent { ControlAction action; int division, index, value; unsigned epoch; };
    RingBuffer<ControlEvent, 1024> _controlEvents; // message-thread producer, audio consumer
    std::atomic<int> _requestedProgram{-1};
    std::atomic<unsigned> _layoutEpoch{0};
    std::atomic<uint64_t> _rejectedControls{0};
    std::atomic<unsigned> _stateSequence{0};
    int _stateWriteDepth = 0; // single audio/setup owner
    struct StateWrite {
        Engine& engine;
        explicit StateWrite(Engine& e) : engine(e) { if (engine._stateWriteDepth++ == 0) engine._stateSequence.fetch_add(1); }
        ~StateWrite() { if (--engine._stateWriteDepth == 0) engine._stateSequence.fetch_add(1); }
    };
    juce::AudioProcessor* _owningProcessor = nullptr; // setup-assigned, lifetime contains Engine
    void processControlEvents(int budget = 8);
    juce::var collectPersistentState() const;
    friend class Division;
    struct PendingVoice {
        Division* division = nullptr;
        Pipewave::State state;
        int stopIndex = 0, quantum = -1;
    };
    struct DivisionSnapshot {
        std::bitset<TOTAL_NOTES> keys, aggregated, blocked;
        int bassNote = -1;
        bool triggered = false;
    };
    void beginAdmission();
    bool finishAdmission(int actualCount, bool restoreKeys, bool registration);
    void forecastAdmission();
    void prepareControlAdmission(int quantum = -1);
    void commitControlAdmission(Division& division);
    bool stageVoice(Division&, int stopIndex, const Pipewave::State&, int quantum);
    void releaseVoiceOrDefer(Voice*);
    bool isDeferredRelease(const Voice*) const;
    void dispatchNoteOn(int note, int midiChannel);
    void updateKeyGraph();

    void populateDivisions();
    void loadDivisionsFromConfig(juce::InputStream& stream);

    void clearDivisionsTriggerFlag();

    bool processSubFrame();

    //------------------------------------------------------
    // Batch renderer (AEOLUS_DSP_WORKERS); see docs/MULTICORE.md.

    struct BatchJob
    {
        Voice* first = nullptr;
        int count = 0;
        float* left = nullptr;
        float* right = nullptr;
        juce::uint64 alive = 0;     ///< Bit q set when a voice of the job sounds in quantum q.
    };

    /// Divisions handled by note on/off (layouts allow at most 10).
    static constexpr int MaxNoteDivisions = 10; // supported organ layout maximum

    static constexpr int MaxBatchQuanta = 32;
    static constexpr int MaxBatchDivisions = 64;

    void prepareBatchRenderer();
    int countSubFramesNeeded(int numFrames) const;
    void nextBatchedSubFrame(int numFrames);
    void renderBatch(int numQuanta);
    void buildBatchJobs(int groupSize);
    static void runBatchJob(void* context, int jobIndex);
    void reportRealtimeStatus();

    void processPendingNoteEvents();
    void prepareReverb();

    /// Generate tremulant osc waveform for a subframe.
    void generateTremulant();

    /// Apply the gloval volume.
    void applyVolume(juce::AudioBuffer<float>& out, int start, int count);
    void applyVolume(float* outL, float* outR, int numFrames);

    /// Process control MIDI messages: program change (sequencer) and stop buttons CC.
    void processControlMIDIMessage(const juce::MidiMessage& message);

    /// Process stop buttons MIDI controls.
    void processStopControlMessage();

    bool isKeySwitchForward(int key) const;
    bool isKeySwitchBackward(int key) const;

    float _sampleRate;

    RingBuffer<NoteEvent, 1024> _pendingNoteEvents;
    std::atomic<bool> _noteQueueOverflow { false };

    RealtimeConfig _rtConfig;       ///< Must precede the voice pool.
    VoicePool _voicePool;           ///< All the voices.
    std::vector<PendingVoice> _pendingVoices; // Sized once, never grown on audio thread.
    std::vector<Voice*> _deferredReleases;
    std::array<DivisionSnapshot, MaxNoteDivisions> _admissionSnapshots{};
    std::array<std::bitset<TOTAL_NOTES>, MaxNoteDivisions> _admissionTouched{};
    int _pendingVoiceCount = 0, _deferredReleaseCount = 0, _controlVoiceCount = 0;
    bool _collectingAdmission = false, _admissionOverflow = false, _controlAdmissionReady = false, _committingAdmission = false;
    std::atomic<uint64_t> _rejectedNotes{0}, _rejectedRegistrations{0};
    std::atomic<unsigned> _registrationRevision{0};
    unsigned _admittedRegistrationRevision = 0;
    int _admittedVoiceLimit = 0;


    AudioParameterPool _params;     ///< Internal parameters.

    std::optional<StopControlMode> _stopControlMode{};
    int _stopControlGroup{};
    int _stopControlButton{};

    bool _invalidConfiguration = false;
    bool _invalidStateFile = false;
    mutable juce::String _lastError; // message/setup thread only
    int _windowWidth { 1420 };
    int _windowHeight { 640 };

    /// List of all divisions
    juce::OwnedArray<Division> _divisions;
    juce::OwnedArray<Division> _retiredDivisions;
    void retireDivisions();
    int _pedalIndex = -1; // selected during setup
    std::vector<Division*> _renderDivisions; // rebuilt only during suspended setup

    std::unique_ptr<Sequencer> _sequencer;

    std::vector<int> _sequencerStepBackwardKeySwitches{ SEQUENCER_BACKWARD_MIDI_KEY };
    std::vector<int> _sequencerStepForwardKeySwitches{ SEQUENCER_FORWARD_MIDI_KEY };

    juce::AudioBuffer<float> _subFrameBuffer;
    juce::AudioBuffer<float> _divisionFrameBuffer;
    juce::AudioBuffer<float> _voiceFrameBuffer;

    int _remainedSamples;

    juce::AudioBuffer<float> _tremulantBuffer;
    float _tremulantPhase;

    dsp::Limiter::Spec _limiterSpec;
    std::array<dsp::Limiter::State, aeolus::N_OUTPUT_CHANNELS> _limiterState;
    dsp::Limiter _limiter;
    std::atomic<bool> _limiterEnabled{};

    dsp::ReverbSwitch _reverb;
    std::mutex _reverbPreparationMutex;
    int _preparedIR = -1;
    std::atomic<int> _selectedIR;

    dsp::Interpolator _interpolator;

    std::unique_ptr<DspTeam> _dspTeam;
    std::vector<BatchJob> _batchJobs;           ///< Capacity reserved up front.
    std::vector<int> _batchDivisionJobs;        ///< First job of each division, plus end.
    std::vector<float> _batchGroupStorage;
    float* _batchGroupBase = nullptr;
    int _batchJobCapacity = 0;
    int _batchQuantaInJob = 0;                  ///< Quanta in the batch being rendered.
    juce::AudioBuffer<float> _batchOutput;
    int _batchQuanta = 0;
    int _batchNext = 0;
    juce::uint64 _batchMispredictions = 0;
    int _reportedAudioGeneration = 0;
    bool _reportedWorkers = false;

    // Only the audio owner clears voices, after every worker has returned.
    void applyEmergencyPanic();
    std::atomic<uint64_t> _audioStarted {0}, _audioCompleted {0}, _guiProgress {0};
    std::atomic<unsigned> _audioState {0}; // 0: deliberately closed, 1: active, 2: preparing
    std::atomic<unsigned> _panicRequested {0}, _panicAcknowledged {0};
    std::atomic<bool> _panicMuted {false};
    std::atomic<unsigned> _resumeAuthorized {~0u};

#if AEOLUS_TESTING
    std::atomic<unsigned> _panicTestDelay {0}, _panicTestStarts {0};
#endif
    juce::MidiKeyboardState _midiKeyboardState;

    Level _volumeLevel;

    std::atomic<int> _midiControlChannelsMask;
    std::atomic<int> _midiSwellChannelsMask;

    std::atomic<bool> _restoringState { false };
    std::atomic<bool> _saveRequested { false };
    int _saveCountdown = 0;
    void timerCallback() override;

        void injectKeepAlive(float* outL, float* outR, int numFrames);

        int _samplesSinceAudio { 0 };
        int _keepAliveSamplesLeft { 0 };
        bool _streamNeedsPrime { true };
        uint32_t _keepAliveRng { 1 };
        float _outputFade { 0.0f };
        bool _startupFadeDone { false };
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(Engine)
};

AEOLUS_NAMESPACE_END
