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

#include "aeolus/setuplock.h"
#include "aeolus/engine.h"
#include "aeolus/paniccontrol.h"
#include <algorithm>
#include <thread>
#include <chrono>
#include <vector>
#include <cstdint>
#include <cstring>

using namespace juce;

AEOLUS_NAMESPACE_BEGIN

//==============================================================================

class PrepareRankwaveJob : ThreadPoolJob
{
public:

    PrepareRankwaveJob(Rankwave* rw, float sampleRate)
        : ThreadPoolJob(rw->getStopName())
        , _rankwave{rw}
        , _sampleRate{sampleRate}

    {
    }

    JobStatus runJob() override
    {
        _rankwave->prepareToPlay(_sampleRate);
        return JobStatus::jobHasFinished;
    }

private:
    Rankwave* _rankwave;
    float _sampleRate;
};


//==============================================================================

namespace settings {
const static char* tuningFrequency = "tuningFrequency";
const static char* tuningTemperament = "tuningTemperament";
const static char* mtsEnabled = "mtsEnabled";
const static char* uiScalingFactor = "uiScalingFactor";
const static char* keyboardVisible = "keyboardVisible";
}

EngineGlobal::EngineGlobal()
    : _rankwaves{}
    , _scale(Scale::EqualTemp)
    , _tuningFrequency(TUNING_FREQUENCY_DEFAULT)
    , _globalProperties{}
    , _mtsClient{ nullptr }
{
    _mtsClient = MTS_RegisterClient();

    PropertiesFile::Options options{};

#if AEOLUS_TESTING
    if (SystemStats::getEnvironmentVariable("AEOLUS_TEST_DATA_DIRECTORY", {}).isNotEmpty())
        options.folderName = getTestDataDirectory().getChildFile("Settings").getFullPathName();
#endif
    options.applicationName = ProjectInfo::projectName;
    options.filenameSuffix = ".settings";
    //options.osxLibrarySubFolder = "~/Library/Application Support";
    options.osxLibrarySubFolder = "Application Support";
    options.storageFormat = PropertiesFile::storeAsXML;

    _globalProperties.setStorageParameters(options);

    loadSettings();

    loadRankwaves();
    loadIRs();

    startTimer(100);
}

EngineGlobal::~EngineGlobal()
{
    if (_mtsClient != nullptr)
        MTS_DeregisterClient(_mtsClient);

    saveSettings(false); // no async dialogs during DeletedAtShutdown teardown
    clearSingletonInstance();
}

void EngineGlobal::registerProcessorProxy(ProcessorProxy* proxy)
{
    jassert(proxy != nullptr);
    _processors.addIfNotAlreadyThere(proxy);
}

void EngineGlobal::unregisterProcessorProxy(ProcessorProxy* proxy)
{
    jassert(proxy != nullptr);
    _processors.removeAllInstancesOf(proxy);
}

void EngineGlobal::addListener(Listener* listener)
{
    jassert(listener != nullptr);
    _listeners.add(listener);
}

void EngineGlobal::removeListener(Listener* listener)
{
    jassert(listener != nullptr);
    _listeners.remove(listener);
}

void EngineGlobal::loadSettings()
{
    if (auto* propertiesFile = _globalProperties.getUserSettings()) {
        const float tuningFreq = (float)propertiesFile->getDoubleValue(settings::tuningFrequency, TUNING_FREQUENCY_DEFAULT);

        if (tuningFreq >= TUNING_FREQUENCY_MIN && tuningFreq <= TUNING_FREQUENCY_MAX)
            _tuningFrequency = tuningFreq;

        const int scaleType = propertiesFile->getIntValue(settings::tuningTemperament, (int)Scale::EqualTemp);

        if (scaleType >= (int)Scale::First && scaleType < (int)Scale::Total)
            _scale.setType(static_cast<Scale::Type>(scaleType));

        setMTSEnabled(propertiesFile->getBoolValue(settings::mtsEnabled, false));

        const float uiScalingFactor = (float)propertiesFile->getDoubleValue(settings::uiScalingFactor, UI_SCALING_DEFAULT);
        if (uiScalingFactor >= UI_SCALING_MIN && uiScalingFactor <= UI_SCALING_MAX)
            _uiScalingFactor = uiScalingFactor;

        _keyboardVisible = propertiesFile->getBoolValue(settings::keyboardVisible, true);
        setVoiceLimitSetting(propertiesFile->getIntValue("voiceLimit", 0));
        setMeterWarningPercent(propertiesFile->getIntValue("meterWarningPercent", 75));
        setMeterCriticalPercent(propertiesFile->getIntValue("meterCriticalPercent", 90));
    }
}

void EngineGlobal::saveSettings(bool reportErrors)
{
    if (auto* propertiesFile = _globalProperties.getUserSettings()) {
        propertiesFile->setValue(settings::tuningFrequency, _tuningFrequency);
        propertiesFile->setValue(settings::tuningTemperament, (int)_scale.getType());
        propertiesFile->setValue(settings::mtsEnabled, _mtsEnabled);
        propertiesFile->setValue(settings::uiScalingFactor, _uiScalingFactor);
        propertiesFile->setValue(settings::keyboardVisible, _keyboardVisible);
        propertiesFile->setValue("voiceLimit", getVoiceLimitSetting());
        propertiesFile->setValue("meterWarningPercent", getMeterWarningPercent());
        propertiesFile->setValue("meterCriticalPercent", getMeterCriticalPercent());
    }

    if (!_globalProperties.saveIfNeeded()) {
        const juce::String error = "Could not save application preferences; changes remain in memory.";
        if (reportErrors) reportUserError("Settings", error);
        else juce::Logger::writeToLog(error);
    }
}

StringArray EngineGlobal::getAllStopNames() const
{
    StringArray names;

    for (const auto* const rankwave : _rankwaves)
        names.add(rankwave->getStopName());

    return names;
}

Rankwave* EngineGlobal::getStopByName(const String& name)
{
    if (!_rankwavesByName.contains(name))
        return nullptr;

    return _rankwavesByName[name];
}

void EngineGlobal::updateStops(float sampleRate)
{
    const std::lock_guard<std::mutex> registryLock(_rankwavesMutex);
    _sampleRate = sampleRate;

    ThreadPool threadPool;
    std::atomic<int> done((int)_rankwaves.size());
    WaitableEvent wait;

    for (auto* rw : _rankwaves) {
        threadPool.addJob([sampleRate, rw, &done, &wait]() {
                rw->prepareToPlay(sampleRate);
                done -= 1;
                wait.signal();
            });
    }

    while (done.load() > 0)
        wait.wait();
    // A job decrements done before signaling wait. Join the job wrappers before
    // either captured stack object can be destroyed (setup thread only).
    threadPool.removeAllJobs(false,-1);

/*
    // Single-thread equivalent
    for (auto* rw : _rankwaves)
        rw->prepareToPlay(sampleRate);
*/
}

bool EngineGlobal::isConnectedToMTSMaster()
{
    if (_mtsClient != nullptr)
        return MTS_HasMaster(_mtsClient);

    return false;
}

String EngineGlobal::getMTSScaleName()
{
    if (_mtsClient == nullptr)
        return {};

    return String(MTS_GetScaleName(_mtsClient));
}

float EngineGlobal::getMTSNoteToFrequency(int midiNote, int midiChannel)
{
#if AEOLUS_TESTING
    if (const float base = _testMTSFrequencyBase.load(); base > 0) return base + midiNote * 1.25f;
#endif
    if (_mtsClient == nullptr || !isConnectedToMTSMaster())
    {
        return _scale.getFrequencyForMidoNote(midiNote, _tuningFrequency);
    }

    return (float)MTS_NoteToFrequency(_mtsClient, (char)midiNote, (char)midiChannel);
}

bool EngineGlobal::shouldMTSFilterNote(int midiNote, int midiChannel)
{
    if (_mtsClient == nullptr || !isConnectedToMTSMaster())
        return false;

    return MTS_ShouldFilterNote(_mtsClient, (char)midiNote, (char)midiChannel);
}

void EngineGlobal::setMTSEnabled(bool shouldBeEnabled)
{
    _mtsEnabled = shouldBeEnabled;

    if (_mtsEnabled && _mtsClient == nullptr) {
        _mtsClient = MTS_RegisterClient();
    } else if (!_mtsEnabled && _mtsClient != nullptr) {
        MTS_DeregisterClient(_mtsClient);
        _mtsClient = nullptr;
    }
}

void EngineGlobal::setUIScalingFactor(float f)
{
    _uiScalingFactor = jlimit(UI_SCALING_MIN, UI_SCALING_MAX, f);
    _listeners.call([&](Listener& listener){ listener.onUIScalingFactorChanged(_uiScalingFactor); });
}

void EngineGlobal::rebuildRankwaves()
{
    // Prepare all the rankwaves to be retuned
    for (auto* rw : _rankwaves) {
        rw->retunePipes(_scale, _tuningFrequency);
    }

    // Existing states retain immutable pipe generations until their tails finish.

    updateStops(_sampleRate);
}

void EngineGlobal::loadRankwaves()
{
    auto& model = *Model::getInstance();

    for (int i = 0; i < model.getStopsCount(); ++i) {
        auto* synth = model[i];
        jassert(synth);

        auto rankwave = std::make_unique<Rankwave>(*synth);
        rankwave->createPipes(_scale, _tuningFrequency);

        auto* ptr = rankwave.get();
        _rankwaves.add(rankwave.release());
        _rankwavesByName.set(ptr->getStopName(), ptr);
    }
}

void EngineGlobal::loadIRs()
{
    _irs.clear();

    // Here we offset the IRs predelay for non-zero convolution instead.
    constexpr bool zeroDelay{ true };

    _irs.push_back({
        "York Guildhall Council Chamber",
        BinaryData::york_council_chamber_wav,
        BinaryData::york_council_chamber_wavSize,
        0.25f,
        zeroDelay,
        zeroDelay ? 0 : 216,
        {}
    });

    _irs.push_back({
        "St Laurentius, Molenbeek",
        BinaryData::st_laurentius_molenbeek_wav,
        BinaryData::st_laurentius_molenbeek_wavSize,
        0.8f,
        zeroDelay,
        zeroDelay ? 0 : 15,
        {}
    });

    _irs.push_back({
        "St Andrew's Church",
        BinaryData::st_andrews_church_wav,
        BinaryData::st_andrews_church_wavSize,
        1.0f,
        zeroDelay,
        zeroDelay ? 0 : 1796,
        {}
    });

    _irs.push_back({
        "St George's Episcopal Church",
        BinaryData::st_georges_far_wav,
        BinaryData::st_georges_far_wavSize,
        1.0f,
        zeroDelay,
        zeroDelay ? 0 : 1776,
        {}
    });

    _irs.push_back({
        "Lady Chapel, St Albans Cathedral",
        BinaryData::lady_chapel_stalbans_wav,
        BinaryData::lady_chapel_stalbans_wavSize,
        1.0f,
        zeroDelay,
        zeroDelay ? 0 : 385,
        {}
    });

    _irs.push_back({
        "1st Baptist Church, Nashville",
        BinaryData::_1st_baptist_nashville_balcony_wav,
        BinaryData::_1st_baptist_nashville_balcony_wavSize,
        1.0f,
        zeroDelay,
        zeroDelay ? 0 : 1764,
        {}
    });

    _irs.push_back({
        "Elveden Hall, Suffolk",
        BinaryData::elveden_hall_suffolk_england_wav,
        BinaryData::elveden_hall_suffolk_england_wavSize,
        0.1f,
        false,  // This one is delayed on purpose
        0,      // 28
        {}
    });

    _irs.push_back({
        "R1 Nuclear Reactor Hall",
        BinaryData::r1_nuclear_reactor_hall_wav,
        BinaryData::r1_nuclear_reactor_hall_wavSize,
        0.4f,
        zeroDelay,
        zeroDelay ? 0 : 1995,
        {}
    });

    _irs.push_back({
        "Sports Centre, University of York",
        BinaryData::york_uni_sportscentre_wav,
        BinaryData::york_uni_sportscentre_wavSize,
        0.4f,
        zeroDelay,
        zeroDelay ? 0 : 1309,
        {}
    });

    _irs.push_back({
        "York Minster",
        BinaryData::york_minster_wav,
        BinaryData::york_minster_wavSize,
        0.3f,
        zeroDelay,
        zeroDelay ? 0 : 3098,
        {}
    });

    AudioFormatManager manager;
    manager.registerBasicFormats();

    // A minimum size we can have is a single convolution block.
    _longestIRLength = dsp::Convolver::BlockSize;

    for (auto& ir : _irs) {
        std::unique_ptr<InputStream> stream = std::make_unique<MemoryInputStream>(ir.data, ir.size, false);
        std::unique_ptr<AudioFormatReader> reader{manager.createReaderFor(std::move(stream))};
        ir.waveform.setSize(reader->numChannels, (int)reader->lengthInSamples);
        const auto offset{ (juce::int64)ir.startOffset };
        reader->read(&ir.waveform, 0, (int)(ir.waveform.getNumSamples() - offset), offset, true, true);

        ir.waveform.applyGain(ir.gain);

        _longestIRLength = jmax(_longestIRLength, ir.waveform.getNumSamples());
    }

}

bool EngineGlobal::updateMTSTuningCache()
{
    bool changed{};

    for (int midiNote = 0; midiNote < _mtsTuningCache.size(); ++midiNote) {
        const float f{ getMTSNoteToFrequency(midiNote) };
        if (_mtsTuningCache[midiNote] != f) {
            _mtsTuningCache[midiNote] = f;
            changed = true;
        }
    }

  return changed;
}

void EngineGlobal::pollStopFiles()
{
    if (++_stopPollCounter < 10) // 100ms timer × 10 = ~1 s
        return;
    _stopPollCounter = 0;

    const auto dirs = getStopSearchDirectories();

    const juce::StringArray skip{
        "organ_config.json", "organ_state.json", "default_organ.json"
    };

    for (auto dir : dirs)
    {
        if (!dir.isDirectory())
            continue;

        for (DirectoryEntry entry : RangedDirectoryIterator(dir, false))
        {
            auto file = entry.getFile();
            const auto ext = file.getFileExtension().toLowerCase();
            if (ext != ".json" && ext != ".ae0")
                continue;
            if (skip.contains(file.getFileName()))
                continue;

            const auto key = file.getFullPathName();
            const auto mod = file.getLastModificationTime().toMilliseconds();

            if (! _stopFileModTimes.contains(key))
            {
                _stopFileModTimes.set(key, mod);
                continue; // first sighting — don't reload
            }

            if (_stopFileModTimes[key] == mod)
                continue;

            // Editor may still be writing — wait until the stamp is ≥300 ms old
            if (Time::currentTimeMillis() - mod < 300)
                continue;

            _stopFileModTimes.set(key, mod);
            reloadStopFile(file);
        }
    }
}

namespace {
class ScopedModelEdit final {
    struct State { EngineGlobal::ProcessorProxy* proxy; unsigned audio; };
    std::vector<State> states;
public:
    explicit ScopedModelEdit(const juce::Array<EngineGlobal::ProcessorProxy*>& processors) {
        states.reserve(size_t(processors.size()));
        for(auto* proxy:processors) {
            auto* processor=proxy->getAudioProcessor();
            processor->getCallbackLock().enter();
            states.push_back({proxy,proxy->getEngine().getAudioState()});
            proxy->getEngine().setAudioPreparing();
        }
    }
    ~ScopedModelEdit() {
        for(auto state=states.rbegin();state!=states.rend();++state) {
            state->proxy->getEngine().restoreAudioState(state->audio);
            state->proxy->getAudioProcessor()->getCallbackLock().exit();
        }
    }
};
}

bool EngineGlobal::reloadStopFile(const File& file)
{
    const auto path = file.getFullPathName();
    if (!file.existsAsFile()) { _deferredStopFiles.removeString(path); return false; }
    const auto name = file.getFileNameWithoutExtension();
    auto* existing = getStopByName(name);
    auto voicesActive = [&] {
        for (auto* proxy : _processors) if (proxy->getNumberOfActiveVoices() > 0) return true;
        return false;
    };
    if (existing && voicesActive()) { _deferredStopFiles.addIfNotAlreadyThere(path); return false; }

    // File access, validation, allocations and waveform synthesis are private.
    // Audio (including convolution tails and new MIDI) continues while preparing.
    auto candidate = std::make_shared<Addsynth>();
    if (candidate->readFromFile(file).failed()) {
        reportUserError("Stop model", "Could not load the stop file; the existing model was retained: " + path);
        _deferredStopFiles.removeString(path);
        return false;
    }
    candidate->setStopName(name);
    auto staged = std::make_unique<Rankwave>(*candidate);
    if (isMTSEnabled()) staged->retunePipes(_scale, _tuningFrequency, candidate);
    else staged->createPipes(_scale, _tuningFrequency, candidate);
    staged->prepareToPlay(_sampleRate.load());
    auto* model = Model::getInstance();
    if (existing) {
        auto* original = model->getStopByName(name);
        if (!original) return false;
        Addsynth previous(*original); // retain old strings until callback ownership ends
        existing->reserveModelReplacement();
        {
            ScopedModelEdit edit(_processors);
            if (voicesActive()) { _deferredStopFiles.addIfNotAlreadyThere(path); return false; }
            *original = *candidate;
            existing->adoptPrepared(*staged);
        }
        existing->collectRetired(); // wave/model destruction remains off audio and outside callback ownership
    } else {
        auto* synth = model->addPreparedStop(*candidate);
        if (!synth) return false;
        auto rankwave = std::make_unique<Rankwave>(*synth);
        rankwave->adoptPrepared(*staged);
        auto* ptr = rankwave.get();
        // Device prepare owns callback -> registry. Publication owns registry
        // only, and never acquires a processor callback while holding it.
        const std::lock_guard<std::mutex> registryLock(_rankwavesMutex);
        _rankwaves.add(rankwave.release());
        _rankwavesByName.set(name, ptr);
    }
    _deferredStopFiles.removeString(path);
    return true;
}

void EngineGlobal::timerCallback()
{
    for (auto* rank : _rankwaves) rank->collectRetired();
    if(!_deferredStopFiles.isEmpty()) {
        bool idle=true;
        for(auto* proxy:_processors) idle &= proxy->getNumberOfActiveVoices()==0;
        if(idle) {
            const auto pending=_deferredStopFiles;
            for(const auto& path:pending) reloadStopFile(File(path));
        }
    }
    pollStopFiles();

    if (!_mtsEnabled)
        return;

    auto changed{ updateMTSTuningCache() };

    if (changed)
        rebuildRankwaves();
}


JUCE_IMPLEMENT_SINGLETON(EngineGlobal)

//==============================================================================

Engine::Engine()
    : _sampleRate{SAMPLE_RATE_F}
    , _rtConfig{RealtimeConfig::fromEnvironment()}
    , _voicePool(*this, _rtConfig.maxVoices)
    , _params{NUM_PARAMS}
    , _divisions{}
    , _sequencer{}
    , _subFrameBuffer{N_OUTPUT_CHANNELS, SUB_FRAME_LENGTH}
    , _divisionFrameBuffer{N_OUTPUT_CHANNELS, SUB_FRAME_LENGTH}
    , _voiceFrameBuffer{N_VOICE_CHANNELS, SUB_FRAME_LENGTH}
    , _remainedSamples{0}
    , _tremulantBuffer{1, SUB_FRAME_LENGTH}
    , _tremulantPhase{0.0f}
    , _reverb{}
    , _selectedIR{0}
    , _interpolator{1.0f, N_OUTPUT_CHANNELS}
    , _midiKeyboardState{}
    , _volumeLevel{}
    , _midiControlChannelsMask{ (1 << 16) - 1 }
    , _midiSwellChannelsMask{ (1 << 16) - 1 }
{
    _pendingVoices.resize((size_t) _voicePool.getMaxVoices());
    _deferredReleases.resize((size_t) _voicePool.getMaxVoices());
    _voicePool.setPerVoiceRandom(_rtConfig.perVoiceRandom);
    prepareBatchRenderer();

   populateDivisions();

    // Sequencer can be created only after the divisions have been populated.
    _sequencer = std::make_unique<Sequencer>(*this, SEQUENCER_N_STEPS);

    loadOrganState();
    startTimer(50);
    _panicControl = PanicControl::create(*this);
}

Engine::~Engine()
{
    if(_panicControl) _panicControl->beginShutdown();
    // Hosts may destroy processors off the message thread. Wait for any timer
    // callback already in flight, then unregister before destroying members.
    const SetupMessageLock messageLock;
    stopTimer();

    if (_dspTeam != nullptr) {
        const auto stats = _dspTeam->getStats();

        if (stats.batches > 0) {
            String text;
            text << "Aeolus batch renderer: " << (int64) stats.batches << " batches, "
                 << (int64) stats.jobs << " jobs ("
                 << String(stats.jobs > 0 ? 100.0 * double(stats.jobsByWorkers) / double(stats.jobs) : 0.0, 1)
                 << "% on workers), waited for workers in " << (int64) stats.waits << " batches, mean "
                 << String(stats.waits > 0 ? double(stats.waitNanosTotal) / double(stats.waits) / 1000.0 : 0.0, 1)
                 << " us, max " << String(double(stats.waitNanosMax) / 1000.0, 1) << " us";
            Logger::writeToLog(text);
        }
    }
}

void Engine::reportRealtimeStatus()
{
    // Audio and worker threads publish their scheduling results; report them
    // here on the message thread.
    auto& report = rt::audioThreadReport();
    const int generation = report.generation.load(std::memory_order_acquire);

    if (generation != _reportedAudioGeneration) {
        _reportedAudioGeneration = generation;
        rt::ThreadPolicyStatus status;
        status.priorityError = report.priorityError.load(std::memory_order_relaxed);
        status.affinityError = report.affinityError.load(std::memory_order_relaxed);
        Logger::writeToLog("Aeolus audio thread: " + rt::describe(status, _rtConfig.audioPriority, _rtConfig.audioCpus));
    }

    if (_dspTeam != nullptr && !_reportedWorkers && _dspTeam->haveAllWorkersStarted()) {
        _reportedWorkers = true;
        String text;
        text << "Aeolus batch renderer: " << _dspTeam->getNumWorkers() << " workers, group size "
             << _rtConfig.dspGroupSize << ", voice pool " << _voicePool.getMaxVoices();

        if (_dspTeam->getNumWorkers() > 0)
            text << "; " << _dspTeam->describeWorkers();

        Logger::writeToLog(text);
    }
}

void Engine::beginShutdown() { if(_panicControl) _panicControl->beginShutdown(); }

bool Engine::hasSafetyWatchdog() const { return _panicControl && _panicControl->watchdogReady(); }
bool Engine::safetyWatchdogStarting() const { return _panicControl && _panicControl->watchdogRunning() && !_panicControl->watchdogReady(); }

void Engine::prepareToPlay(float sampleRate, int frameSize)
{
    const auto resumeEpoch=_resumeAuthorized.exchange(~0u,std::memory_order_acq_rel);
    ignoreUnused(frameSize);

    // Make sure the stops wavetable is updated.
    auto* g = EngineGlobal::getInstance();
    g->updateStops(SAMPLE_RATE_F);

    _limiterSpec.threshold = 0.8f;
    _limiterSpec.attack = 1000.0f / sampleRate;
    _limiterSpec.release = 1.0f / sampleRate;
    _limiterSpec.sustain = std::max(0, int(sampleRate * 0.5f));

    for (auto& state : _limiterState)
        dsp::Limiter::resetState(_limiterSpec, state);

    // Playback is stopped during prepareToPlay. Serialize with the UI timer.
    const std::lock_guard<std::mutex> preparationLock(_reverbPreparationMutex);
    _reverb.reset();
    _preparedIR = -1;

    _interpolator.setRatio(SAMPLE_RATE_F / sampleRate); // 44100 / sampleRate
    _interpolator.reset();

    // Discard quanta rendered for the previous stream and restart the
    // per-voice noise seed sequence so that renders are repeatable.
    _batchQuanta = 0;
    _batchNext = 0;
    _remainedSamples = 0;
    _subFrameBuffer.clear();
    _voicePool.restartRandomSequence();

    _sampleRate = sampleRate;

    _samplesSinceAudio = 0;
        _keepAliveSamplesLeft = 0;
        _streamNeedsPrime = false;
        _outputFade = 0.0f;
        _startupFadeDone = false;   

    prepareReverb();
    // Playback is stopped; the reverb and interpolator are now fresh. A panic
    // arriving during preparation must remain latched.
    if (_panicMuted.load(std::memory_order_acquire)) {
        // The UI producer cannot run concurrently with this message-thread
        // device reprepare. Drain any event that raced the initial panic.
        allNotesOff();
        for (auto* division : _divisions) division->emergencySilence();
        for (auto* division : _retiredDivisions) division->emergencySilence();
    }
    if(resumeEpoch!=~0u && resumeEpoch==getPanicRequested() && !isEmergencyPanicPending()) {
        _panicMuted.store(false,std::memory_order_release);
        if(resumeEpoch!=getPanicRequested() || isEmergencyPanicPending())
            _panicMuted.store(true,std::memory_order_release);
    }
}

void Engine::prepareReverb()
{
#if !AEOLUS_MULTIBUS_OUTPUT
    _reverb.collect();
    const int selected = _selectedIR.load();
    const auto& irs = EngineGlobal::getInstance()->getIRs();
    if (selected != _preparedIR && isPositiveAndBelow(selected, int(irs.size()))) {
        const auto& ir = irs[size_t(selected)];
        _reverb.prepare(ir.waveform, ir.zeroDelay, _sampleRate);
        _preparedIR = selected;
    }
#endif
}

void Engine::setReverbIR(int num)
{
    const auto& irs = EngineGlobal::getInstance()->getIRs();
    if (isPositiveAndBelow(num, int(irs.size()))) {
        _selectedIR.store(num);
        requestSaveOrganState();
    }
}

void Engine::postReverbIR(int num)
{
    setReverbIR(num);
}

float Engine::getReverbLengthInSeconds() const
{
    return _reverb.tailSeconds();
}

void Engine::setReverbWet(float v)
{
    _reverb.setWet(v);
}

void Engine::setVolume(float v)
{
    _params[VOLUME].setValue(v);
}

void Engine::enableLimiter(bool shouldBeEnabled)
{
    _limiterEnabled = shouldBeEnabled;
}

bool Engine::isLimiterEnabled() const
{
    return _limiterEnabled;
}

void Engine::process(float* outL, float* outR, int numFrames, bool isNonRealtime)
{
    processControlEvents();
    jassert(outL != nullptr);
    jassert(outR != nullptr);

    float* origOutL = outL;
    float* origOutR = outR;
    int origNumFrames = numFrames;
    if (isEmergencyMuted()) {
        applyEmergencyPanic();
        FloatVectorOperations::clear(origOutL, origNumFrames);
        FloatVectorOperations::clear(origOutR, origNumFrames);
        return;
    }

    processPendingNoteEvents();

    while (numFrames > 0)
    {
        if (_remainedSamples > 0)
        {
            const int idx = SUB_FRAME_LENGTH - _remainedSamples;
            const float* subL = _subFrameBuffer.getReadPointer(0, idx);
            const float* subR = _subFrameBuffer.getReadPointer(1, idx);

            while (_remainedSamples > 0 && _interpolator.canWrite()) {
                _interpolator.write(*subL, *subR);
                --_remainedSamples;
                subL += 1;
                subR += 1;
            }

            while (numFrames > 0 && _interpolator.canRead()) {
                _interpolator.read(*outL, *outR);
                numFrames -= 1;
                outL += 1;
                outR += 1;
            }
        }

        if (_remainedSamples == 0 && numFrames > 0)
        {
            if (_dspTeam != nullptr)
                nextBatchedSubFrame(numFrames);
            else
                processSubFrame();

            if (isEmergencyMuted()) {
                applyEmergencyPanic();
                FloatVectorOperations::clear(origOutL, origNumFrames);
                FloatVectorOperations::clear(origOutR, origNumFrames);
                return;
            }
            jassert(_remainedSamples > 0);
        }
    }

    // Every rendered quantum should have been consumed: the batch length is
    // predicted exactly. Leftovers would be played by the next call, after
    // its MIDI events, so count them for the tests.
    if (_batchNext < _batchQuanta)
        _batchMispredictions += juce::uint64(_batchQuanta - _batchNext);

    _reverb.process(origOutL, origOutR, origNumFrames, isNonRealtime);

    applyVolume(origOutL, origOutR, origNumFrames);

    // Always dither so WASAPI Low Latency never sees digital silence
    injectKeepAlive(origOutL, origOutR, origNumFrames);

    // Apply limiter
    if (_limiterEnabled) {
        dsp::Limiter::process(_limiterSpec, _limiterState[0], origOutL, origOutL, origNumFrames);
        dsp::Limiter::process(_limiterSpec, _limiterState[1], origOutR, origOutR, origNumFrames);
    }

    if (isEmergencyMuted()) {
        applyEmergencyPanic();
        FloatVectorOperations::clear(origOutL, origNumFrames);
        FloatVectorOperations::clear(origOutR, origNumFrames);
    }
    _volumeLevel.left.process(origOutL, origNumFrames);
    _volumeLevel.right.process(origOutR, origNumFrames);
}

void Engine::process(AudioBuffer<float>& out, bool isNonRealtime, int startSample, int numSamples)
{
    processControlEvents();
    ignoreUnused(isNonRealtime);

    const int numChannels = out.getNumChannels();
    int numFrames = numSamples < 0 ? out.getNumSamples() - startSample : numSamples;

    processPendingNoteEvents();

    if (isEmergencyMuted()) {
        applyEmergencyPanic();
        out.clear(startSample, numFrames);
        return;
    }
    int outIdx = startSample;

    while (numFrames > 0) {
        int idx = SUB_FRAME_LENGTH - _remainedSamples;

        while (_remainedSamples > 0 && _interpolator.canWrite()) {
            for (int ch = 0; ch < numChannels; ++ch)
                _interpolator.writeUnchecked(_subFrameBuffer.getReadPointer(ch)[idx], (size_t) ch);

            _interpolator.writeIncrement();

            _remainedSamples -= 1;
            idx += 1;
        }

        while (numFrames > 0 && _interpolator.canRead()) {
            for (int ch = 0; ch < numChannels; ++ch)
                out.getWritePointer(ch)[outIdx] = _interpolator.readUnchecked(ch);

            _interpolator.readIncrement();

            numFrames -= 1;
            outIdx += 1;
        }

        if (_remainedSamples == 0 && numFrames > 0)
        {
            processSubFrame();
            jassert(_remainedSamples > 0);
        }

    }

    // Multibus processing does not have a convolver FX

    // Global volume across all the buses
    const int rendered = outIdx - startSample;
    if (isEmergencyMuted()) {
        applyEmergencyPanic();
        out.clear(startSample, rendered);
        return;
    }
    applyVolume(out, startSample, rendered);

    _volumeLevel.left.process(out, startSample, rendered);
    _volumeLevel.right = _volumeLevel.left;
}

bool Engine::isPedalDivision(const Division* d) const
{
    if (d == nullptr)
        return false;

    bool anyNamed = false;
    for (auto* x : _divisions)
        if (x->getName().containsIgnoreCase("Pedal"))
            anyNamed = true;

    if (anyNamed)
        return d->getName().containsIgnoreCase("Pedal");

    return _divisions.getLast() == d;
}

Division* Engine::getPedalDivision()
{
    return juce::isPositiveAndBelow(_pedalIndex, getDivisionCount()) ? _divisions[_pedalIndex] : nullptr;
}

namespace {
void restoreEditedLayoutState(Engine& engine, const var& saved)
{
    auto compatible = saved.clone();
    if (auto* object = compatible.getDynamicObject()) {
        if (auto* divisions = object->getProperty("divisions").getArray()) {
            while (divisions->size() > engine.getDivisionCount()) divisions->removeLast();
            while (divisions->size() < engine.getDivisionCount())
                divisions->add(engine.getDivisionByIndex(divisions->size())->getPersistentState());
        }
    }
    engine.setPersistentState(compatible);
}
}

void Engine::applyCouplerLayout(int sourceIndex, bool hasBassCoupler,
                               const std::vector<Division::LinkSpec>& specs)
{
    Array<CouplerEdits> edits;
    CouplerEdits e;
    e.sourceIndex = sourceIndex;
    e.hasBassCoupler = hasBassCoupler;
    e.specs = specs;
    edits.add(std::move(e));
    applyCouplerLayouts(edits);
}

void Engine::applyCouplerLayouts(const Array<CouplerEdits>& edits)
{
    if (_invalidConfiguration) {
        _lastError = "Rejected custom organ configuration was preserved. Repair organ_config.json and restart before editing."; return;
    }
    if (_owningProcessor && getAudioState() != 0 && !_owningProcessor->isSuspended()) {
        _lastError = "Organ edits require suspended processing."; return;
    }

    const auto configFile = getCustomOrganConfigFile();
    if (!configFile.existsAsFile()) { _lastError = "Missing organ_config.json; edit was not applied."; return; }

    var config = JSON::parse(configFile.loadFileAsString());
    auto* obj = config.getDynamicObject();
    if (obj == nullptr) { _lastError = "Cannot read organ_config.json; edit was not applied."; return; }

    auto divisionsVar = obj->getProperty("divisions");
    auto* arr = divisionsVar.getArray();
    if (arr == nullptr) { _lastError = "Invalid organ divisions; edit was not applied."; return; }

    for (const auto& edit : edits)
    {
        if (!isPositiveAndBelow(edit.sourceIndex, arr->size()))
            continue;

        auto* divObj = arr->getReference(edit.sourceIndex).getDynamicObject();
        if (divObj == nullptr)
            continue;

        Array<var> links;
        for (const auto& spec : edit.specs)
        {
            auto* o = new DynamicObject();
            o->setProperty("to", spec.targetName);
                        o->setProperty("kind", spec.octaveShift == 12 ? "super"
                                                : spec.octaveShift == -12 ? "sub"
                                                : "unison");
                        o->setProperty("octave", spec.octaveShift == 0 ? 0 : (spec.octaveShift > 0 ? 1 : -1));
                        o->setProperty("semitones", spec.octaveShift == 12 ? 12
                                                    : spec.octaveShift == -12 ? -12
                                                    : 0);
                        o->setProperty("passthrough", spec.passThrough);
            links.add(var(o));
        }

        divObj->setProperty("links", links);
            divObj->setProperty("has_bass_coupler", edit.hasBassCoupler);
        }

        for (int i = 0; i < arr->size(); ++i)
            {
                auto* dobj = arr->getReference(i).getDynamicObject();
                if (dobj == nullptr)
                    continue;

                const String dname = dobj->getProperty("name").toString();
                if (dname.containsIgnoreCase("Pedal"))
                    continue;

                auto* linksArr = dobj->getProperty("links").getArray();
                if (linksArr == nullptr)
                    continue;

                Array<var> kept;
                for (const auto& item : *linksArr)
                {
                    if (auto* o = item.getDynamicObject())
                    {
                        const String to = o->getProperty("to").toString();
                        if (to.containsIgnoreCase("Pedal"))
                            continue;
                    }
                    else if (item.toString().containsIgnoreCase("Pedal"))
                    {
                        continue;
                    }
                    kept.add(item);
                }
                dobj->setProperty("links", kept);
            }

        obj->setProperty("divisions", divisionsVar);
    auto* configDivisions = config.getProperty("divisions", {}).getArray();
    bool valid = configDivisions && configDivisions->size() >= 1 && configDivisions->size() <= 10;
    if (configDivisions) for (const auto& item : *configDivisions) valid &= item.getDynamicObject() != nullptr;
    if (!valid) { _lastError = "Organ configuration must contain 1 to 10 division objects."; return; }
    const var savedState = getPersistentState();
    if (savedState.isVoid()) { _lastError = "Registration snapshot is busy; organ edit was not applied."; return; }
    const auto serializedConfig = JSON::toString(config, true);
    if (saveTextFile(configFile, serializedConfig).failed()) {
        _lastError = "Could not save organ configuration: " + configFile.getFullPathName();
        return;
    }


        retireDivisions();
        {
            MemoryInputStream stream(serializedConfig.toRawUTF8(), serializedConfig.getNumBytesAsUTF8(), false);
            loadDivisionsFromConfig(stream);
        _invalidConfiguration = false;
        }
        for (auto* division : _divisions)
            division->clearLinkedDivisions();
        for (auto* division : _divisions)
            division->populateLinkedDivisions();

        _sequencer->initFromEngine();
        restoreEditedLayoutState(*this, savedState);

        for (auto* division : _divisions)
            division->ensurePresets();
        saveOrganState(); // setup thread: persist migrated layout state immediately
}

void Engine::processMIDIMessage(const MidiMessage& message)
{
    std::optional<StateWrite> write;
    if (message.isController() || message.isProgramChange()) write.emplace(*this);
    // Process global CCs
    if (midi::matchChannelToMask(getMIDIControlChannelsMask(), message.getChannel())) {
        processControlMIDIMessage(message);
    }

    if (message.isController()) {
        // Process divisions CCs
        for (auto* division : _divisions)
            division->handleControlMessage(message);
    } else if (message.isNoteOnOrOff()) {
        // Notes on/off
        _midiKeyboardState.processNextMidiEvent(message);
        return;
    }
}

namespace {
template<size_t N>
void propagateAdmissionKeys(Engine& engine, std::array<std::bitset<TOTAL_NOTES>, N>& wanted)
{
    const int count = jmin(engine.getDivisionCount(), int(N));
    // Only roots and arrivals through pass-through links can propagate again.
    // Each (division,note) is visited at most once. Self octave links produce
    // one extra pitch, never an octave cascade.
    auto forwarding = wanted;
    std::array<int, N * TOTAL_NOTES> queue; // only initialized entries are read
    int read = 0, write = 0;
    for (int d = 0; d < count; ++d)
        for (int note = 0; note < TOTAL_NOTES; ++note)
            if (forwarding[(size_t)d][(size_t)note]) queue[(size_t)write++] = d * TOTAL_NOTES + note;
    while (read < write) {
        const int key = queue[(size_t)read++], source = key / TOTAL_NOTES, note = key % TOTAL_NOTES;
        auto& src = *engine.getDivisionByIndex(source);
        for (int l = 0; l < src.getLinksCount(); ++l) {
            const auto& link = src.getLinkByIndex(l);
            if (!link.enabled) continue;
            int target = 0;
            while (target < count && engine.getDivisionByIndex(target) != link.division) ++target;
            const int coupled = note + link.octaveShift;
            if (target == count || !juce::isPositiveAndBelow(coupled, TOTAL_NOTES)) continue;
            wanted[(size_t)target].set((size_t)coupled);
            if (target != source && link.passThrough && !forwarding[(size_t)target][(size_t)coupled]) {
                forwarding[(size_t)target].set((size_t)coupled);
                queue[(size_t)write++] = target * TOTAL_NOTES + coupled;
            }
        }
    }
}
}

int Engine::getVoiceLimit() const noexcept
{
    const int requested = EngineGlobal::getInstance()->getVoiceLimitSetting();
    return requested == 0 ? _voicePool.getMaxVoices() : jmin(requested, _voicePool.getMaxVoices());
}

void Engine::beginAdmission()
{
    jassert(!_collectingAdmission);
    _collectingAdmission = true;
    _admissionOverflow = getDivisionCount() > MaxNoteDivisions;
    for (int i = 0; i < _pendingVoiceCount; ++i) _pendingVoices[(size_t)i].state.reset();
    _pendingVoiceCount = _deferredReleaseCount = 0;
    for (int i = 0; i < jmin(getDivisionCount(), MaxNoteDivisions); ++i) {
        auto& d = *_divisions[i];
        _admissionSnapshots[(size_t)i] = {d._keysState, d._aggregatedKeysState,
            d._capacityBlockedKeys, d._bassCouplerNote, d._triggerFlag};
        _admissionTouched[(size_t)i].reset();
        for (auto& notes : d._admissionVoiced) notes.reset();
        for (auto& notes : d._admissionStaged) notes.reset();
        for (auto* voice = d._activeVoices.first(); voice; voice = voice->next()) {
            const int stop = voice->stopIndex(), note = voice->getNote();
            if (voice->isActive() && isPositiveAndBelow(stop, d._admissionVoiced.size())
                && isPositiveAndBelow(note, TOTAL_NOTES))
                d._admissionVoiced[(size_t)stop].set((size_t)note);
        }
    }
}

bool Engine::isDeferredRelease(const Voice* voice) const
{
    for (int i = 0; i < _deferredReleaseCount; ++i)
        if (_deferredReleases[(size_t)i] == voice) return true;
    return false;
}

void Engine::releaseVoiceOrDefer(Voice* voice)
{
    if (!_collectingAdmission) { voice->release(); return; }
    if (!isDeferredRelease(voice)) {
        jassert(_deferredReleaseCount < (int)_deferredReleases.size());
        _deferredReleases[(size_t)_deferredReleaseCount++] = voice;
    }
}

bool Engine::stageVoice(Division& division, int stopIndex, const Pipewave::State& state, int quantum)
{
    if (_collectingAdmission) {
        if (getDivisionCount() > MaxNoteDivisions) return false;
        for (int i = 0; i < jmin(getDivisionCount(), MaxNoteDivisions); ++i)
            if (_divisions[i] == &division)
                _admissionTouched[(size_t)i].set((size_t)state.pipewave->getNote());
        if (_pendingVoiceCount == (int)_pendingVoices.size()) { _admissionOverflow = true; return false; }
        _pendingVoices[(size_t)_pendingVoiceCount++] = {&division, state, stopIndex, quantum};
        division._admissionStaged[(size_t)stopIndex].set((size_t)state.pipewave->getNote());
        return true;
    }
    // Guard direct/internal calls as well. Normal dispatch always preflights a
    // whole transaction; this guard never steals a voice or truncates a tail.
    if (!_committingAdmission && getVoiceCount() >= getVoiceLimit()) return false;
    if (auto* voice = _voicePool.trigger(state)) {
        if (quantum > 0) voice->setBatchStart(quantum);
        voice->setStopIndex(stopIndex);
        division._activeVoices.append(voice);
        return true;
    }
    return false;
}

void Engine::forecastAdmission()
{
    // The actual dispatcher and control pass use the same complete graph.
    for (auto* d : _divisions) for (int note = 0; note < TOTAL_NOTES; ++note)
        if (d->_aggregatedKeysState[(size_t)note]) d->triggerNoteInternal(note);
}

bool Engine::finishAdmission(int actualCount, bool restoreKeys, bool registration)
{
    const bool accepted = !_admissionOverflow &&
        _pendingVoiceCount <= jmax(0, getVoiceLimit() - getVoiceCount());
    _collectingAdmission = false;
    for (int i = 0; i < jmin(getDivisionCount(), MaxNoteDivisions); ++i) {
        auto& d = *_divisions[i];
        const auto& saved = _admissionSnapshots[(size_t)i];
        if (restoreKeys || !accepted) {
            d._keysState = saved.keys;
            d._aggregatedKeysState = saved.aggregated;
            d._capacityBlockedKeys = saved.blocked;
            d._bassCouplerNote = saved.bassNote;
            d._triggerFlag = saved.triggered;
        }
        if (!accepted && registration) d._capacityBlockedKeys |= _admissionTouched[(size_t)i];
    }
    if (!accepted) {
        if (registration) _rejectedRegistrations.fetch_add(1, std::memory_order_relaxed);
        else _rejectedNotes.fetch_add(1, std::memory_order_relaxed);
    } else if (!registration) {
        for (int i = 0; i < _deferredReleaseCount; ++i) _deferredReleases[(size_t)i]->release();
        _committingAdmission = true;
        for (int i = 0; i < actualCount; ++i) {
            const auto& p = _pendingVoices[(size_t)i];
            stageVoice(*p.division, p.stopIndex, p.state, p.quantum);
        }
        _committingAdmission = false;
    }
    _controlVoiceCount = accepted && registration ? actualCount : 0;
    for (int i = _controlVoiceCount; i < _pendingVoiceCount; ++i) _pendingVoices[(size_t)i].state.reset();
    if (!registration) _pendingVoiceCount = 0;
    return accepted;
}

void Engine::prepareControlAdmission(int quantum)
{
    updateKeyGraph();
    beginAdmission();
    // Release scheduling and DSP remain in the original control/render order.
    for (auto* division : _divisions) division->controlStep(quantum);
    const int actualCount = _pendingVoiceCount;
    const unsigned revision = _registrationRevision.load(std::memory_order_relaxed);
    const int limit = getVoiceLimit();
    if (revision != _admittedRegistrationRevision || limit != _admittedVoiceLimit)
        forecastAdmission();
    _admittedRegistrationRevision = revision;
    _admittedVoiceLimit = limit;
    finishAdmission(actualCount, true, true);
    _controlAdmissionReady = true;
}

void Engine::commitControlAdmission(Division& division)
{
    _committingAdmission = true;
    for (int i = 0; i < _controlVoiceCount; ++i) {
        const auto& p = _pendingVoices[(size_t)i];
        if (p.division == &division) {
            stageVoice(division, p.stopIndex, p.state, p.quantum);
            _pendingVoices[(size_t)i].state.reset();
        }
    }
    _committingAdmission = false;
}

void Engine::noteOn(int note, int midiChannel)
{
    if (isEmergencyMuted()) return;
    clearDivisionsTriggerFlag();

    bool handled = false;
    if (midi::matchChannelToMask(getMIDIControlChannelsMask(), midiChannel)) {
        if (isKeySwitchBackward(note)) { StateWrite write(*this); _sequencer->stepBackward(); handled = true; }
        else if (isKeySwitchForward(note)) { StateWrite write(*this); _sequencer->stepForward(); handled = true; }
    }
    if (handled)
        return;

    auto* g = aeolus::EngineGlobal::getInstance();
    if (g->shouldMTSFilterNote(note, midiChannel))
        return;

    if (note < 0 || note >= TOTAL_NOTES) return;
    std::array<uint16_t, MaxNoteDivisions> previous{};
    const int channel = jlimit(1, 16, midiChannel) - 1;
    for (int i = 0; i < getDivisionCount(); ++i) previous[(size_t)i] = _divisions[i]->_held[(size_t)channel][(size_t)note];
    updateKeyGraph(); // exclude prior control/MIDI graph edits from this note's delta
    beginAdmission();
    dispatchNoteOn(note, midiChannel);
    const int actualCount = _pendingVoiceCount;
    if (!finishAdmission(actualCount, false, false))
        for (int i = 0; i < getDivisionCount(); ++i) _divisions[i]->_held[(size_t)channel][(size_t)note] = previous[(size_t)i];
}

void Engine::updateKeyGraph()
{
    std::array<std::bitset<TOTAL_NOTES>, MaxNoteDivisions> wanted{};
    const int count = jmin(getDivisionCount(), MaxNoteDivisions);
    int pedal = -1;
    for (int i = 0; i < count; ++i) {
        wanted[(size_t)i] = _divisions[i]->_keysState;
        if (_divisions[i] == getPedalDivision()) pedal = i;
    }
    if (pedal >= 0) for (int i = 0; i < count; ++i) {
        auto& d = *_divisions[i];
        d._bassCouplerNote = -1;
        if (i == pedal || !d.isBassCouplerEnabled()) continue;
        for (int note = 0; note < TOTAL_NOTES; ++note) if (d._keysState[(size_t)note]) {
            wanted[(size_t)pedal].set((size_t)note);
            d._bassCouplerNote = note;
            break;
        }
    }
    propagateAdmissionKeys(*this, wanted);
    for (int i = 0; i < count; ++i) _divisions[i]->_aggregatedKeysState = wanted[(size_t)i];
}

void Engine::dispatchNoteOn(int note, int midiChannel)
{
    for (auto* d : _divisions) d->noteOn(note, midiChannel, false);
    updateKeyGraph();
    // A note transaction owns only newly reached keys. Unrelated held-key
    // registration additions are admitted separately at the control quantum.
    for (int i = 0; i < getDivisionCount(); ++i) {
        auto* d = _divisions[i];
        const auto added = d->_aggregatedKeysState & ~_admissionSnapshots[(size_t)i].aggregated;
        for (int key = 0; key < TOTAL_NOTES; ++key)
            if (added[(size_t)key]) d->triggerNoteInternal(key);
    }
}

void Engine::noteOff(int note, int midiChannel)
{
    if (!isPositiveAndBelow(note, TOTAL_NOTES)) return;
    for (auto* d : _divisions) d->noteOff(note, midiChannel, false);
    updateKeyGraph();
    for (auto* d : _divisions) {
        d->_capacityBlockedKeys &= d->_aggregatedKeysState;
        d->releaseVoicesOfDisabledStops();
    }
}

void Engine::applyEmergencyPanic()
{
    const unsigned epoch = _panicRequested.load(std::memory_order_acquire);
    if (epoch == _panicAcknowledged.load(std::memory_order_acquire)) return;
    _panicMuted.store(true, std::memory_order_release);
    allNotesOff();
    for (auto* division : _divisions) division->emergencySilence();
    for (auto* division : _retiredDivisions) division->emergencySilence();
    _batchQuanta = _batchNext = _remainedSamples = 0;
    _subFrameBuffer.clear();
    _interpolator.reset();
    // Reverb tails remain muted until explicit device reprepare off the callback.
    _panicAcknowledged.store(epoch, std::memory_order_release);
}

void Engine::allNotesOff()
{
    // Panic also cancels notes queued before it. Otherwise a GUI note-on can
    // be replayed immediately after the keyboard and divisions were cleared.
    _noteQueueOverflow.store(false);
    NoteEvent discarded;
    const auto queued = _pendingNoteEvents.count();
    for (size_t i = 0; i < queued; ++i)
        _pendingNoteEvents.receive(discarded);

    for (auto* division : _divisions)
        division->allNotesOff();

    _midiKeyboardState.allNotesOff(0);
}

Range<int> Engine::getMidiKeyboardRange() const
{
    int minNote = -1;
    int maxNote = -1;

    for (auto* division : _divisions) {
        int min, max;
        division->getAvailableRange(min, max);

        if (min >= 0 && max >= 0) {
            if (minNote < 0 || minNote > min)
                minNote = min;

            if (maxNote < 0 || maxNote < max)
                maxNote = max;
        }
    }

    return Range<int>(minNote, maxNote);
}

std::set<int> Engine::getKeySwitches() const
{
    std::set<int> keySwitches{};

    for (int key : _sequencerStepBackwardKeySwitches)
        keySwitches.insert(key);

    for (int key : _sequencerStepForwardKeySwitches)
        keySwitches.insert(key);

    return keySwitches;
}

Division* Engine::getDivisionByName(const String& name)
{
    for (auto* division : _divisions) {
        if (division->getName() == name)
            return division;
    }

    return nullptr;
}

var Engine::getPersistentState() const
{
    for (int attempt = 0; attempt < 64; ++attempt) {
        const auto before = _stateSequence.load();
        if (before & 1) { std::this_thread::yield(); continue; }
        auto state = collectPersistentState();
        if (_stateSequence.load() == before) return state;
        std::this_thread::yield();
    }
    const_cast<Engine*>(this)->requestSaveOrganState();
    return {}; // never write stale state; timer retries, host snapshots suspend first
}

var Engine::collectPersistentState() const
{
    auto* obj = new DynamicObject();

    // Save control channel
    obj->setProperty("midi_ctrl_channels_mask", getMIDIControlChannelsMask());
    obj->setProperty("midi_swell_channels_mask", getMIDISwellChannelsMask());

    // Save the IR.
    int irNum = _selectedIR;
    obj->setProperty("ir", irNum);

    // Save divisions.
    Array<var> divisions;

    for (auto* division : _divisions)
        divisions.add(division->getPersistentState());

    obj->setProperty("divisions", divisions);

    obj->setProperty("sequencer", _sequencer->getPersistentState());

    obj->setProperty("window_width", _windowWidth);
    obj->setProperty("window_height", _windowHeight);

    return var{obj};
}

bool Engine::canRestoreState(const var& state) const
{
    const auto* object = state.getDynamicObject();
    if (!object) return false;
    if (object->hasProperty("divisions")) {
        const auto* divisions = object->getProperty("divisions").getArray();
        if (!divisions || divisions->size() != getDivisionCount()) return false;
        for (const auto& division : *divisions) if (!division.getDynamicObject()) return false;
    }
    return true;
}

void Engine::setPersistentState(const var& state)
{
    if (!canRestoreState(state)) return;
    if (const auto* obj = state.getDynamicObject()) {
        if (auto* divisions = obj->getProperty("divisions").getArray())
            if (divisions->size() != getDivisionCount()) return;
        // Restore control channels

        if (const auto& v = obj->getProperty("window_width"); !v.isVoid())
            _windowWidth = jmax(640, (int)v);

        if (const auto& v = obj->getProperty("window_height"); !v.isVoid())
            _windowHeight = jmax(480, (int)v);

        if (const auto& v = obj->getProperty("midi_ctrl_channel"); !v.isVoid()) {
            int ch = (int)v;
            if (ch == 0)
                setMIDIControlChannelsMask((1 << 16) - 1);
            else
                setMIDIControlChannelsMask(midi::channelToMask(ch));
        } else {
            setMIDIControlChannelsMask(obj->getProperty("midi_ctrl_channels_mask"));
        }

        if (const auto& v = obj->getProperty("midi_swell_channel"); !v.isVoid()) {
            int ch = (int)v;
            if (ch == 0)
                setMIDISwellChannelsMask((1 << 16) - 1);
            else
                setMIDISwellChannelsMask(midi::channelToMask(ch));
        } else {
            setMIDISwellChannelsMask(obj->getProperty("midi_swell_channels_mask"));
        }

        // Restore the IR
        int irNum = obj->getProperty("ir");

        postReverbIR(irNum);

        // Restore the sequencer
        _sequencer->setPersistentState(obj->getProperty("sequencer"));

        // Restore the divisions after the sequencer (in case we are restoring
        // from a state that did not have a sequencer before).
        if (const auto* divisions = obj->getProperty("divisions").getArray()) {

            if (divisions->size() != _divisions.size()) {
                DBG("Saved state is invalid and will be ignored");
                return;
            }

            for (int divIdx = 0; divIdx < _divisions.size(); ++divIdx) {
                auto* division = _divisions.getUnchecked(divIdx);
                division->setPersistentState(divisions->getReference(divIdx));
            }
        }

    }
}

void Engine::loadOrganState()
{
    const auto stateFile = getOrganStateFile();

    if (!stateFile.existsAsFile())
        return;

    if (stateFile.getSize() == 0) return; // no prior state to preserve
    const auto text = stateFile.loadFileAsString();
    const auto state = JSON::parse(text);
    if (!canRestoreState(state)) {
        _invalidStateFile = true;
        if (_lastError.isNotEmpty() && !_lastError.endsWithChar('\n')) _lastError += "\n";
        _lastError += "Invalid or incompatible organ_state.json was preserved; saving is disabled for this session. Repair or remove it and restart to enable saving.";
        return;
    }

    if (auto* obj = state.getDynamicObject())
    {
        if (obj->getProperties().isEmpty())
            return;
    }

    _restoringState = true;
    setPersistentState(state);
    _restoringState = false;
}

void Engine::saveOrganState() const
{
    if (_invalidConfiguration || _invalidStateFile) return; // preserve the state belonging to the rejected custom organ
    const auto stateFile = getOrganStateFile();
    const auto state = getPersistentState();
    if (state.isVoid()) { const_cast<Engine*>(this)->requestSaveOrganState(); return; }
    const auto text = JSON::toString(state, true); // pretty-print

    if (saveTextFile(stateFile, text).failed()) {
        _lastError = "Could not save organ state: " + stateFile.getFullPathName();
    }
}

void Engine::requestSaveOrganState()
{
    if (!_restoringState.load())
        _saveRequested.store(true);
}

void Engine::timerCallback()
{
    _guiProgress.fetch_add(1, std::memory_order_release);
    // Audio/MIDI code only sets a flag. Timer lifetime is tied to the engine;
    // no queued lambda can dereference an engine after destruction.
    reportRealtimeStatus();
    if (_lastError.isNotEmpty()) {
        auto error = _lastError; _lastError.clear();
        aeolus::reportUserError( "Aeolus", error);
    }

    {
        const std::lock_guard<std::mutex> preparationLock(_reverbPreparationMutex);
        prepareReverb();
    }
    if (_saveRequested.exchange(false))
        _saveCountdown = 5;
    else if (_saveCountdown > 0 && --_saveCountdown == 0 && !_restoringState.load())
        saveOrganState();
}

void Engine::populateDivisions()
{
    if (auto result = ensureOrganDataFiles(); result.failed()) _lastError += "Configuration setup: " + result.getErrorMessage() + "\n";
    if (auto* model = Model::getInstance(); model->getLoadError().isNotEmpty()) _lastError += "Stop data: " + model->getLoadError() + "\n";

    const auto configFile = getCustomOrganConfigFile();

    if (configFile.existsAsFile()) {
        FileInputStream stream(configFile);
        loadDivisionsFromConfig(stream);
    } else {
        MemoryInputStream stream(BinaryData::default_organ_json,
                                 BinaryData::default_organ_jsonSize, false);
        loadDivisionsFromConfig(stream);
    }

    if (_divisions.isEmpty()) {
        _invalidConfiguration = true;
        _lastError += "Invalid organ configuration; using the embedded organ without overwriting your files.\n";
        MemoryInputStream fallback(BinaryData::default_organ_json, BinaryData::default_organ_jsonSize, false);
        loadDivisionsFromConfig(fallback);
    }
    // Remove all the links if any.
    for (auto* division : _divisions) {
        division->clearLinkedDivisions();
    }

    // Update division links after they've been loaded.
    for (auto* division : _divisions) {
        division->populateLinkedDivisions();
    
    for (auto* division : _divisions)
        division->ensurePresets();
    }
    auto infer = [](const String& pipe) -> Stop::Type
        {
            const auto n = pipe.toLowerCase();
            if (n.contains("trumpet") || n.contains("trombone") || n.contains("bassoon")
                || n.contains("oboe") || n.contains("clarinet") || n.contains("cromhorne")
                || n.contains("vox") || n.contains("bombarde") || n.contains("cornopean")
                || n.contains("posaune") || n.contains("fagott") || n.contains("krumm"))
                return Stop::Type::Reed;
            if (n.contains("gamba") || n.contains("violin") || n.contains("viola")
                || n.contains("celest") || n.contains("salicion"))
                return Stop::Type::String;
            if (n.contains("flute") || n.contains("bourdon") || n.contains("rohr")
                || n.contains("nasard") || n.contains("nazard") || n.contains("tierce")
                || n.contains("larigot") || n.contains("sifflet") || n.contains("gemshorn")
                || n.contains("melodia") || n.contains("quintaden"))
                return Stop::Type::Flute;
            return Stop::Type::Unknown;
        };

    for (auto* division : _divisions)
    {
        for (int i = 0; i < division->getStopsCount(); ++i)
        {
            auto& stop = division->getStopByIndex(i);
            if (stop.getZones().empty() || stop.getZones()[0].rankwaves.empty())
                continue;

            const auto inferred = infer(stop.getZones()[0].rankwaves[0]->getStopName());
            if (inferred != Stop::Type::Unknown)
                stop.setType(inferred);
        }
    }
}

static juce::String romanDivisionName(int indexZeroBased)
{
    static const char* r[] = { "I", "II", "III", "IV", "V", "VI", "VII", "VIII", "IX", "X" };
    return juce::String("Division ") + r[juce::jlimit(0, 9, indexZeroBased)];
}

void Engine::applyDivisionLayout(int count, const juce::StringArray& names)
{
    if (_owningProcessor && getAudioState() != 0 && !_owningProcessor->isSuspended()) {
        _lastError = "Organ edits require suspended processing."; return;
    }

    count = jlimit(1, 10, count);

    const auto configFile = getCustomOrganConfigFile();

    if (!configFile.existsAsFile()) { _lastError = "Missing organ_config.json; edit was not applied."; return; }
    var config = JSON::parse(configFile.loadFileAsString());

    auto* obj = config.getDynamicObject();
    if (obj == nullptr) { _lastError = "Cannot read organ_config.json; edit was not applied."; return; }

    auto* arr = obj->getProperty("divisions").getArray();
    bool sourceValid = arr && arr->size() >= 1 && arr->size() <= 10;
    if (arr) for (const auto& division : *arr) sourceValid &= division.getDynamicObject() != nullptr;
    if (_invalidConfiguration || !sourceValid) {
        _lastError = "Rejected custom organ configuration was preserved. Repair organ_config.json and restart before editing.";
        return;
    }

    while (arr->size() > count)
        arr->remove(arr->size() - 1);

    while (arr->size() < count)
    {
        auto* d = new DynamicObject();
        const int i = arr->size();
        d->setProperty("name", romanDivisionName(i));
        d->setProperty("mnemonic", String(i + 1));
        d->setProperty("swell", false);
        d->setProperty("tremulant", true);
        d->setProperty("tremulant_level", 0.2);
        d->setProperty("stops", Array<var>());
        arr->add(var(d));
    }

    for (int i = 0; i < count; ++i)
    {
        auto* d = arr->getReference(i).getDynamicObject();
        if (d == nullptr)
            continue;

        String name = (i < names.size()) ? names[i].trim() : String();
        if (name.isEmpty())
            name = romanDivisionName(i);

        d->setProperty("name", name);
    }

    auto* configDivisions = config.getProperty("divisions", {}).getArray();
    bool valid = configDivisions && configDivisions->size() >= 1 && configDivisions->size() <= 10;
    if (configDivisions) for (const auto& item : *configDivisions) valid &= item.getDynamicObject() != nullptr;
    if (!valid) { _lastError = "Organ configuration must contain 1 to 10 division objects."; return; }
    const var savedState = getPersistentState();
    if (savedState.isVoid()) { _lastError = "Registration snapshot is busy; organ edit was not applied."; return; }
    const auto serializedConfig = JSON::toString(config, true);
    if (saveTextFile(configFile, serializedConfig).failed()) {
        _lastError = "Could not save organ configuration: " + configFile.getFullPathName();
        return;
    }


    retireDivisions();
    {
        MemoryInputStream stream(serializedConfig.toRawUTF8(), serializedConfig.getNumBytesAsUTF8(), false);
        loadDivisionsFromConfig(stream);
        _invalidConfiguration = false;
    }
    for (auto* division : _divisions)
        division->clearLinkedDivisions();
    for (auto* division : _divisions)
        division->populateLinkedDivisions();

    _sequencer->initFromEngine();
    restoreEditedLayoutState(*this, savedState);

    for (auto* division : _divisions)
        division->ensurePresets();
    saveOrganState(); // setup thread: persist migrated layout state immediately
}

bool Engine::applyDivisionStops(int divisionIndex, const StringArray& pipeNames, bool reload, var* pendingConfig)
{
    if (_invalidConfiguration) {
        _lastError = "Rejected custom organ configuration was preserved. Repair organ_config.json and restart before editing."; return false;
    }
    if (_owningProcessor && getAudioState() != 0 && !_owningProcessor->isSuspended()) {
        _lastError = "Organ edits require suspended processing."; return false;
    }


    const auto configFile = getCustomOrganConfigFile();
    if (!configFile.existsAsFile()) { _lastError = "Missing organ_config.json; edit was not applied."; return false; }
    var config;
    if (pendingConfig) config = pendingConfig->clone();
    else if (configFile.existsAsFile())
        config = JSON::parse(configFile.loadFileAsString());
    else { _lastError = "Missing organ_config.json; edit was not applied."; return false; }

    auto* obj = config.getDynamicObject();
    if (obj == nullptr) { _lastError = "Cannot read organ_config.json; edit was not applied."; return false; }

    auto divisionsVar = obj->getProperty("divisions");
    auto* arr = divisionsVar.getArray();
    if (arr == nullptr || !isPositiveAndBelow(divisionIndex, arr->size())) {
        _lastError = "Stop edit targets a division missing from organ_config.json; nothing was applied."; return false;
    }

    auto* divObj = arr->getReference(divisionIndex).getDynamicObject();
    if (divObj == nullptr) {
        _lastError = "Invalid division object; stop edit was not applied."; return false;
    }

    Array<var> stops;
    auto* model = Model::getInstance();

    auto inferType = [](const String& pipe) -> String
    {
        const auto n = pipe.toLowerCase();
        if (n.contains("trumpet") || n.contains("trombone") || n.contains("bassoon")
            || n.contains("oboe") || n.contains("clarinet") || n.contains("cromhorne")
            || n.contains("vox") || n.contains("bombarde") || n.contains("cornopean")
            || n.contains("posaune") || n.contains("fagott") || n.contains("krumm"))
            return "reed";
        if (n.contains("gamba") || n.contains("violin") || n.contains("viola")
            || n.contains("celest") || n.contains("salicion"))
            return "string";
        if (n.contains("flute") || n.contains("bourdon") || n.contains("rohr")
            || n.contains("nasard") || n.contains("nazard") || n.contains("tierce")
            || n.contains("larigot") || n.contains("sifflet") || n.contains("gemshorn")
            || n.contains("melodia") || n.contains("quintaden"))
            return "flute";
        return "principal";
    };

    auto firstPipeOf = [](const var& stopVar) -> String
    {
        if (auto* o = stopVar.getDynamicObject())
        {
            const auto p = o->getProperty("pipe");
            if (p.isArray() && p.getArray()->size() > 0)
                return (*p.getArray())[0].toString();
            if (p.isString())
                return p.toString();
            if (auto* zones = o->getProperty("zones").getArray(); zones != nullptr && zones->size() > 0)
                if (auto* z = zones->getUnchecked(0).getDynamicObject())
                {
                    const auto zp = z->getProperty("pipe");
                    if (zp.isArray() && zp.getArray()->size() > 0)
                        return (*zp.getArray())[0].toString();
                    return zp.toString();
                }
        }
        return {};
    };

    const Array<var>* oldStops = divObj->getProperty("stops").getArray();

    for (const auto& pipe : pipeNames)
    {
        if (pipe.isEmpty() || pipe == "(none)")
            continue;

        var matched;
        if (oldStops != nullptr)
        {
            for (const auto& oldStop : *oldStops)
            {
                const auto oldPipe = firstPipeOf(oldStop);
                if (oldPipe == pipe || pipe.startsWith(oldPipe + "__"))
                {
                    matched = oldStop;
                    break;
                }
            }
        }

        // Unchanged pipe (including mixtures) — keep type, name, chiff, zones
        if (!matched.isVoid() && firstPipeOf(matched) == pipe)
            {
                if (auto* mo = matched.getDynamicObject())
                {
                    const String inferred = inferType(pipe);
                    const String cur = mo->getProperty("type").toString();
                    if (inferred != "principal")
                        mo->setProperty("type", inferred);
                    else if (cur.isEmpty())
                        mo->setProperty("type", "principal");
                }
                stops.add(matched);
                continue;
            }

        auto* synth = model->getStopByName(pipe);
        String footage = "8'";
        if (synth != nullptr)
        {
            const int fn = synth->getFn();
            const int fd = synth->getFd();
            if      (fn == 1 && fd == 4) footage = "32'";
            else if (fn == 1 && fd == 2) footage = "16'";
            else if (fn == 3 && fd == 4) footage = "10 2/3'";
            else if (fn == 1 && fd == 1) footage = "8'";
            else if (fn == 3 && fd == 2) footage = "5 1/3'";
            else if (fn == 2 && fd == 1) footage = "4'";
            else if (fn == 3 && fd == 1) footage = "2 2/3'";
            else if (fn == 4 && fd == 1) footage = "2'";
            else if (fn == 5 && fd == 1) footage = "1 3/5'";
            else if (fn == 6 && fd == 1) footage = "1 1/3'";
            else if (fn == 8 && fd == 1) footage = "1'";
        }

        String display = pipe;
        String type = inferType(pipe);
        float chiff = 0.5f;

        if (auto* mo = matched.getDynamicObject())
        {
            type = mo->getProperty("type").toString();
            if (mo->hasProperty("chiff"))
                chiff = (float)mo->getProperty("chiff");
            display = mo->getProperty("name").toString().upToFirstOccurrenceOf("\n", false, false);
        }
        else if (synth != nullptr && synth->getMnemonic().isNotEmpty())
        {
            display = synth->getMnemonic();
        }

        auto* s = new DynamicObject();
        s->setProperty("name", display + "\n" + footage);
        s->setProperty("type", type);
        s->setProperty("pipe", pipe);
        s->setProperty("chiff", chiff);
        stops.add(var(s));
    }

    divObj->setProperty("stops", stops);
    obj->setProperty("divisions", divisionsVar);
    if (pendingConfig && !reload) { *pendingConfig = config; return true; }
    auto* configDivisions = config.getProperty("divisions", {}).getArray();
    bool valid = configDivisions && configDivisions->size() >= 1 && configDivisions->size() <= 10;
    if (configDivisions) for (const auto& item : *configDivisions) valid &= item.getDynamicObject() != nullptr;
    if (!valid) { _lastError = "Organ configuration must contain 1 to 10 division objects."; return false; }
    const var savedState = getPersistentState();
    if (savedState.isVoid()) { _lastError = "Registration snapshot is busy; organ edit was not applied."; return false; }
    const auto serializedConfig = JSON::toString(config, true);
    if (saveTextFile(configFile, serializedConfig).failed()) {
        _lastError = "Could not save organ configuration: " + configFile.getFullPathName();
        return false;
    }

    if (!reload)
        return true;


    retireDivisions();
    {
        MemoryInputStream stream(serializedConfig.toRawUTF8(), serializedConfig.getNumBytesAsUTF8(), false);
        loadDivisionsFromConfig(stream);
        _invalidConfiguration = false;
    }
    for (auto* division : _divisions)
        division->clearLinkedDivisions();
    for (auto* division : _divisions)
        division->populateLinkedDivisions();

    _sequencer->initFromEngine();
    restoreEditedLayoutState(*this, savedState);

    for (auto* division : _divisions)
        division->ensurePresets();
    saveOrganState(); // setup thread: persist migrated layout state immediately
    return true;
}

bool Engine::applyDivisionStopsList(const Array<DivisionStopEdits>& edits)
{
    if (edits.isEmpty()) return true;

    auto config = JSON::parse(getCustomOrganConfigFile().loadFileAsString());
    if (!config.isObject() || !config.getProperty("divisions",{}).isArray()) {
        _lastError = "Cannot read organ_config.json; stop edits were not applied."; return false;
    }
    for (const auto& division : *config.getProperty("divisions",{}).getArray()) if (!division.getDynamicObject()) {
        _lastError = "Invalid division object; stop edits were not applied."; return false;
    }
    for (const auto& edit : edits) if (!isPositiveAndBelow(edit.divisionIndex, getDivisionCount())) {
        _lastError = "Stop edit targets a missing division; nothing was applied."; return false;
    }
    for (int i = 0; i < edits.size(); ++i)
        if (!applyDivisionStops(edits.getReference(i).divisionIndex,
                           edits.getReference(i).pipeNames,
                           i == edits.size() - 1, &config)) return false;
    return true;
}

// @internal Helper to populate key switches from a single number or a list
static void populateKeySwitchesVector(std::vector<int>& switches, const var& v)
{
    if (v.isVoid())
        return;

    switches.clear();

    if (v.isInt()) {
        switches.push_back((int)v);
    } else if (v.isArray()) {
        if (auto* a = v.getArray()) {
            for (const auto& key : *a)
                switches.push_back((int)key);
        }
    }
}

void Engine::loadDivisionsFromConfig(InputStream& stream)
{
    // Load organ config JSON
    auto config = JSON::parse(stream);

    if (auto* divisions = config.getProperty("divisions", {}).getArray()) {
        if (divisions->isEmpty() || divisions->size() > 10) {
            _invalidConfiguration = true;
            _lastError += "Organ configuration must contain 1 to 10 divisions.\n";
            return;
        }
        for (const auto& item : *divisions) if (!item.getDynamicObject()) {
            _invalidConfiguration = true; _lastError += "Invalid division object in organ configuration.\n"; return;
        }
        for (int i = 0; i < divisions->size(); ++i) {
            if (auto* divisionObj = divisions->getUnchecked(i).getDynamicObject()) {
                auto division = std::make_unique<Division>(*this);

                division->initFromVar(divisions->getUnchecked(i));

                _divisions.add(division.release());
            }
        }
    }

    if (auto* sequencer = config.getProperty("sequencer", {}).getDynamicObject()) {
        if (var v = sequencer->getProperty("backward_key"); !v.isVoid())
            populateKeySwitchesVector(_sequencerStepBackwardKeySwitches, v);

        if (var v = sequencer->getProperty("forward_key"); !v.isVoid())
            populateKeySwitchesVector(_sequencerStepForwardKeySwitches, v);
    }
    _pedalIndex = -1;
    for (int i = 0; i < _divisions.size(); ++i) if (_divisions[i]->getName().containsIgnoreCase("Pedal")) { _pedalIndex = i; break; }
    if (_pedalIndex < 0 && !_divisions.isEmpty()) _pedalIndex = _divisions.size()-1;
    _renderDivisions.clear();
    for (auto* division : _divisions) _renderDivisions.push_back(division);
    for (auto* division : _retiredDivisions) _renderDivisions.push_back(division);
}

void Engine::clearDivisionsTriggerFlag()
{
    for (auto* division : _divisions)
        division->clearTriggerFlag();
}

void Engine::postNoteEvent(bool onOff, int note, int midiChannel)
{
    if (isEmergencyMuted()) return;
    if (!_pendingNoteEvents.send({onOff, note, midiChannel}))
        _noteQueueOverflow.store(true);
}

void Engine::retireDivisions()
{
    _layoutEpoch.fetch_add(1);
    // Setup thread, with processing suspended by the editor. Destruction and
    // vector growth stay here; the callback only renders/recycles tail voices.
    for (int i = _retiredDivisions.size(); --i >= 0;)
        if (_retiredDivisions[i]->getActiveVoices().first() == nullptr)
            _retiredDivisions.remove(i);
    while (!_divisions.isEmpty()) {
        auto* division = _divisions.removeAndReturn(0);
        division->retire();
        _retiredDivisions.add(division);
    }
    _renderDivisions.clear();
    for (auto* d : _retiredDivisions) _renderDivisions.push_back(d);
    _pedalIndex = -1;
}

bool Engine::processSubFrame()
{
    jassert(_subFrameBuffer.getNumChannels() == _divisionFrameBuffer.getNumChannels());
    jassert(_subFrameBuffer.getNumSamples() == _divisionFrameBuffer.getNumSamples());

    generateTremulant();

    _subFrameBuffer.clear();

    prepareControlAdmission();
    bool wasAudioGenerated = false;
    auto renderDivision = [&](Division* division) {

        _divisionFrameBuffer.clear();

        const bool hasVoices = division->process(_divisionFrameBuffer, _voiceFrameBuffer);
        wasAudioGenerated |= hasVoices;

        if (hasVoices) {
            division->modulate(_divisionFrameBuffer, _tremulantBuffer);

            for (int ch = 0; ch < _subFrameBuffer.getNumChannels(); ++ch)
                _subFrameBuffer.addFrom(ch, 0, _divisionFrameBuffer, ch, 0, SUB_FRAME_LENGTH);
        }

#if AEOLUS_MULTIBUS_OUTPUT
        division->volumeLevel().left.process(_divisionFrameBuffer);
        division->volumeLevel().right = division->volumeLevel().left;
#else
        division->volumeLevel().left.process(_divisionFrameBuffer, 0);
        division->volumeLevel().right.process(_divisionFrameBuffer, 1);
#endif
    };

    for (auto* division : _divisions) renderDivision(division);
    for (auto* division : _retiredDivisions)
        if (division->getActiveVoices().first()) renderDivision(division);

    _remainedSamples = SUB_FRAME_LENGTH;

    return wasAudioGenerated;
}

//==============================================================================
// Batch renderer
//
// Renders several 64-sample quanta at once. Division control steps (keys,
// releases, triggers) still run serially for every quantum, in the original
// order, before any audio is rendered: releases are scheduled for their
// quantum and new voices start at theirs. Voices are then rendered whole for
// the batch, in groups of consecutive voices of one division, by the audio
// thread and the DSP team. Group outputs are summed in a fixed order and
// division modulation runs serially per quantum. Finished voices return to
// the pool at the end of the batch. See docs/MULTICORE.md for the exact
// differences from the serial renderer.

void Engine::prepareBatchRenderer()
{
    if (!_rtConfig.isBatchRendering())
        return;

    const int groupSize = _rtConfig.dspGroupSize;
    _batchJobCapacity = (_rtConfig.maxVoices + groupSize - 1) / groupSize + MaxBatchDivisions;
    _batchJobs.reserve((size_t) _batchJobCapacity);
    _batchDivisionJobs.reserve(MaxBatchDivisions + 1);

    constexpr size_t stride = MaxBatchQuanta * SUB_FRAME_LENGTH;
    constexpr size_t alignFloats = 16; // 64 bytes
    _batchGroupStorage.assign((size_t) _batchJobCapacity * 2 * stride + alignFloats, 0.0f);

    auto address = reinterpret_cast<std::uintptr_t>(_batchGroupStorage.data());
    address = (address + 63) & ~std::uintptr_t(63);
    _batchGroupBase = reinterpret_cast<float*>(address);

    _batchOutput.setSize(N_OUTPUT_CHANNELS, (int) stride);
    _batchOutput.clear();

    _dspTeam = std::make_unique<DspTeam>(_rtConfig.dspWorkers, _rtConfig.workerCpus,
                                         _rtConfig.audioPriority, &Engine::runBatchJob, this);
}

int Engine::countSubFramesNeeded(int numFrames) const
{
    // Mirror the consumption loop in process(), starting at a quantum
    // boundary, using the interpolator's exact arithmetic.
    const float ratio = _interpolator.getRatio();
    float fraction = _interpolator.getFraction();
    int remained = 0;
    int count = 0;

    while (numFrames > 0) {
        if (remained > 0) {
            while (remained > 0 && fraction >= 1.0f) {
                fraction -= 1.0f;
                --remained;
            }

            while (numFrames > 0 && fraction < 1.0f) {
                fraction += ratio;
                --numFrames;
            }
        }

        if (remained == 0 && numFrames > 0) {
            ++count;
            remained = SUB_FRAME_LENGTH;
        }
    }

    return count;
}

void Engine::nextBatchedSubFrame(int numFrames)
{
    if (_batchNext >= _batchQuanta) {
        // No events can arrive before this call returns, so a long request
        // may safely be split into several batches.
        renderBatch(jlimit(1, jmin((int) MaxBatchQuanta, _rtConfig.maxBatchQuanta), countSubFramesNeeded(numFrames)));
    }

    for (int ch = 0; ch < _subFrameBuffer.getNumChannels(); ++ch)
        FloatVectorOperations::copy(_subFrameBuffer.getWritePointer(ch),
                                    _batchOutput.getReadPointer(ch, _batchNext * SUB_FRAME_LENGTH),
                                    SUB_FRAME_LENGTH);

    ++_batchNext;
    _remainedSamples = SUB_FRAME_LENGTH;
}

void Engine::buildBatchJobs(int groupSize)
{
    constexpr size_t stride = MaxBatchQuanta * SUB_FRAME_LENGTH;

    for (;;) {
        _batchJobs.clear();
        _batchDivisionJobs.clear();
        bool overflow = false;

        for (auto* division : _renderDivisions) {
            _batchDivisionJobs.push_back((int) _batchJobs.size());
            auto* voice = division->getActiveVoices().first();

            while (voice != nullptr && !overflow) {
                if ((int) _batchJobs.size() >= _batchJobCapacity) {
                    overflow = true;
                    break;
                }

                BatchJob job;
                job.first = voice;
                job.left = _batchGroupBase + _batchJobs.size() * 2 * stride;
                job.right = job.left + stride;

                while (voice != nullptr && job.count < groupSize) {
                    ++job.count;
                    voice = voice->next();
                }

                _batchJobs.push_back(job);
            }
        }

        if (!overflow)
            break;

        // Only possible with far more divisions than configured layouts
        // allow; larger groups always fit because capacity exceeds the
        // division count.
        jassertfalse;
        groupSize *= 2;
    }

    _batchDivisionJobs.push_back((int) _batchJobs.size());
}

void Engine::runBatchJob(void* context, int jobIndex)
{
    auto& engine = *static_cast<Engine*>(context);
#if AEOLUS_TESTING
    if (const auto delay = engine._panicTestDelay.load(std::memory_order_relaxed); delay && !engine.isEmergencyMuted()) {
        engine._panicTestStarts.fetch_add(1, std::memory_order_release);
        std::this_thread::sleep_for(std::chrono::microseconds(delay));
    }
#endif
    auto& job = engine._batchJobs[(size_t) jobIndex];
    const int numQuanta = engine._batchQuantaInJob;
    const size_t numSamples = (size_t) numQuanta * SUB_FRAME_LENGTH;

    std::memset(job.left, 0, sizeof(float) * numSamples);
    std::memset(job.right, 0, sizeof(float) * numSamples);

    juce::uint64 alive = 0;
    auto* voice = job.first;

    for (int i = 0; i < job.count && voice != nullptr && !engine.isEmergencyMuted(); ++i) {
        auto* next = voice->next();
        const int start = voice->getBatchStart();
        const int end = voice->renderBatch(job.left, job.right, numQuanta);

        if (end > start)
            alive |= ((juce::uint64(1) << end) - 1) & ~((juce::uint64(1) << start) - 1);

        voice = next;
    }

    job.alive = alive;
}

void Engine::renderBatch(int numQuanta)
{
    jassert(numQuanta > 0 && numQuanta <= MaxBatchQuanta);

    _batchQuanta = 0;
    _batchNext = 0;

    // Previous jobs and output consumption have finished. Compact only this
    // non-owning list before building the next batch's division indices; retired
    // owners are still destroyed by suspended setup. Empty retired divisions
    // cannot gain voices again, while empty live divisions must retain control.
    _renderDivisions.erase(std::remove_if(_renderDivisions.begin(), _renderDivisions.end(),
        [](Division* division) {
            return division->_retired && division->getActiveVoices().first() == nullptr;
        }), _renderDivisions.end());

    if (_renderDivisions.size() > MaxBatchDivisions) {
        // Unsupported layout: render serially into the batch buffer.
        // Several generations of release tails may exceed the grouped layout capacity.

        for (int q = 0; q < numQuanta; ++q) {
            processSubFrame();

            for (int ch = 0; ch < _batchOutput.getNumChannels(); ++ch)
                FloatVectorOperations::copy(_batchOutput.getWritePointer(ch, q * SUB_FRAME_LENGTH),
                                            _subFrameBuffer.getReadPointer(ch), SUB_FRAME_LENGTH);
        }

        _batchQuanta = numQuanta;
        return;
    }

    // A voice that finishes leaves its division's list in the serial renderer,
    // which later control steps observe (a stop with no remaining voices for
    // held keys is retriggered). End the batch at the first quantum in which
    // any voice finishes, so finished voices are reclaimed at the same point.
    for (auto* division : _renderDivisions)
        for (auto* voice = division->getActiveVoices().first(); voice != nullptr; voice = voice->next())
            numQuanta = jmin(numQuanta, voice->predictFinishQuantum(numQuanta) + 1);

    // Control steps for every quantum, in serial order. Releases scheduled
    // here may shorten the batch further.
    for (int q = 0; q < numQuanta; ++q) {
        prepareControlAdmission(q);
        for (auto* division : _renderDivisions)
            division->controlStep(q, &numQuanta);
    }

    // Render all voices for the whole batch.
    buildBatchJobs(_rtConfig.dspGroupSize);
    _batchQuantaInJob = numQuanta;
    _dspTeam->run((int) _batchJobs.size());
    if (isEmergencyMuted()) {
        // All in-flight jobs have now acknowledged; discard the cancelled batch.
        _batchOutput.clear();
        _batchQuanta = numQuanta;
        return;
    }

    // Mix and modulate each quantum, as processSubFrame() does.
    for (int q = 0; q < numQuanta; ++q) {
        generateTremulant();
        _subFrameBuffer.clear();

        for (int d = 0; d < (int)_renderDivisions.size(); ++d) {
            auto* division = _renderDivisions[(size_t)d];
            if (division->_retired && _batchDivisionJobs[(size_t)d] == _batchDivisionJobs[(size_t)d+1]) continue;
            _divisionFrameBuffer.clear();
            bool hasVoices = false;

            for (int j = _batchDivisionJobs[(size_t) d]; j < _batchDivisionJobs[(size_t) d + 1]; ++j) {
                const auto& job = _batchJobs[(size_t) j];

                if (((job.alive >> q) & 1) == 0)
                    continue;

                hasVoices = true;
                const size_t offset = (size_t) q * SUB_FRAME_LENGTH;
                FloatVectorOperations::add(_divisionFrameBuffer.getWritePointer(0), job.left + offset, SUB_FRAME_LENGTH);
                FloatVectorOperations::add(_divisionFrameBuffer.getWritePointer(1), job.right + offset, SUB_FRAME_LENGTH);
            }

            if (hasVoices) {
                division->modulate(_divisionFrameBuffer, _tremulantBuffer);

                for (int ch = 0; ch < _subFrameBuffer.getNumChannels(); ++ch)
                    _subFrameBuffer.addFrom(ch, 0, _divisionFrameBuffer, ch, 0, SUB_FRAME_LENGTH);
            }

            division->volumeLevel().left.process(_divisionFrameBuffer, 0);
            division->volumeLevel().right.process(_divisionFrameBuffer, 1);
        }

        for (int ch = 0; ch < _batchOutput.getNumChannels(); ++ch)
            FloatVectorOperations::copy(_batchOutput.getWritePointer(ch, q * SUB_FRAME_LENGTH),
                                        _subFrameBuffer.getReadPointer(ch), SUB_FRAME_LENGTH);
    }

    // Return finished voices to the pool.
    for (auto* division : _renderDivisions) {
        auto& voices = division->getActiveVoices();
        auto* voice = voices.first();

        while (voice != nullptr) {
            if (voice->isOver()) {
                auto* next = voices.removeAndReturnNext(voice);
                voice->resetAndReturnToPool();
                voice = next;
            } else {
                voice->endBatch();
                voice = voice->next();
            }
        }
    }

    _batchQuanta = numQuanta;
}

void Engine::processPendingNoteEvents()
{
    NoteEvent event;
    if (_noteQueueOverflow.exchange(false)) {
        // Discarding a partial stream is safer than replaying note-ons after
        // their note-offs were lost. Panic drains a bounded queue snapshot.
        allNotesOff();
        return;
    }

    const auto queued = _pendingNoteEvents.count();
    for (size_t i = 0; i < queued && _pendingNoteEvents.receive(event); ++i) {
        if (event.on)
            noteOn(event.note, event.midiChannel);
        else
            noteOff(event.note, event.midiChannel);
    }
}

void Engine::generateTremulant()
{
    float* buf = _tremulantBuffer.getWritePointer(0);
    jassert(buf != nullptr);

    for (int i = 0; i < SUB_FRAME_LENGTH; ++i) {
        const float s = sinf(_tremulantPhase);
        buf[i] = s * TREMULANT_LEVEL;
        _tremulantPhase += TREMULANT_PHASE_INCREMENT;

        if (_tremulantPhase >= juce::MathConstants<float>::twoPi)
            _tremulantPhase -= juce::MathConstants<float>::twoPi;
    }
}

void Engine::applyVolume(AudioBuffer<float>& out, int start, int count)
{
    if (_params[VOLUME].isSmoothing()) {
        for (int i = start; i < start + count; ++i) {
            const float g = _params[VOLUME].nextValue() * VOLUME_GAIN;

            for (int ch = 0; ch < out.getNumChannels(); ++ch)
                out.getWritePointer(ch)[i] *= g;
        }
    } else {
        const float g = _params[VOLUME].target() * VOLUME_GAIN;
        out.applyGain(start, count, g);
    }
}

void Engine::applyVolume(float* outL, float* outR, int numFrames)
{
    if (_params[VOLUME].isSmoothing()) {
        for (int i = 0; i < numFrames; ++i) {
            const float g = _params[VOLUME].nextValue() * VOLUME_GAIN;
            outL[i] *= g;
            outR[i] *= g;
        }
    } else {
        const float g = _params[VOLUME].target() * VOLUME_GAIN;

        for (int i = 0; i < numFrames; ++i) {
            outL[i] *= g;
            outR[i] *= g;
        }
    }
}

void Engine::processControlMIDIMessage(const MidiMessage& message)
{
    // @note VST3 will not pass the program change MIDI messages through.
    //       Instead program change must be handled at the processor level
    //       via the setCurrentProgram() method.

    // Here we handle the program change message nevertheless
    // in case of a non-VST3 or stand-alone plugin.
    if (message.isProgramChange()) {
        int step = message.getProgramChangeNumber();

        if (step >= 0 && step < _sequencer->getStepsCount())
            _sequencer->setStep(step);
    } else if (message.isController() && message.getControllerNumber() == CC_STOP_BUTTONS) {
        const auto value{ message.getControllerValue() };

        if ((value & 0xC8) == 0x40) {
            // 01mm0ggg
            StopControlMode mode { StopControlMode::Disabled };

            const int modeValue{ (value >> 4) & 0x03 };
            switch (modeValue) {
                case 0: mode = StopControlMode::Disabled; break;
                case 1: mode = StopControlMode::SetOff; break;
                case 2: mode = StopControlMode::SetOn; break;
                case 3: mode = StopControlMode::Toggle; break;
                default: break;
            }

            _stopControlMode = mode;
            _stopControlGroup = value & 0x07;

            if (_stopControlMode == StopControlMode::Disabled) {
                // Disable message does not require a 2nd part and can be processed immeditely.
                processStopControlMessage();

                _stopControlMode.reset();
            }
        } else if ((value & 0xE0) == 0) {
            // 000bbbbb
            if (_stopControlMode.has_value()) {
                _stopControlButton = value & 0x1F;

                processStopControlMessage();
            }
        } else {
            _stopControlMode.reset();
        }
    }
}

void Engine::processStopControlMessage()
{
    if (!_stopControlMode.has_value())
        return;

    if (!juce::isPositiveAndBelow(_stopControlGroup, _divisions.size()))
        return;

    auto* division{ _divisions.getUnchecked(_stopControlGroup) };

    const auto mode{ *_stopControlMode };
    if (mode != StopControlMode::Disabled
        && !isPositiveAndBelow(_stopControlButton, division->getStopsCount()))
        return;

    switch (mode) {
        case StopControlMode::Disabled:
            division->disableAllStops();
            break;
        case StopControlMode::SetOff:
            division->enableStop(_stopControlButton, false);
            break;
        case StopControlMode::SetOn:
            division->enableStop(_stopControlButton, true);
            break;
        case StopControlMode::Toggle:
            division->enableStop(_stopControlButton, !division->isStopEnabled(_stopControlButton));
            break;
        default:
            break;
    }
}

bool Engine::isKeySwitchForward(int key) const
{
    return std::find(_sequencerStepForwardKeySwitches.begin(),
                     _sequencerStepForwardKeySwitches.end(),
                     key) != _sequencerStepForwardKeySwitches.end();
}

bool Engine::isKeySwitchBackward(int key) const
{
    return std::find(_sequencerStepBackwardKeySwitches.begin(),
                     _sequencerStepBackwardKeySwitches.end(),
                     key) != _sequencerStepBackwardKeySwitches.end();
}

void Engine::setMIDIControlChannelsMask(int mask)
{
    _midiControlChannelsMask = mask & 0xffff;
    requestSaveOrganState();
}

void Engine::setMIDISwellChannelsMask(int mask)
{
    _midiSwellChannelsMask = mask & 0xffff;
    requestSaveOrganState();
}

void Engine::injectKeepAlive(float* outL, float* outR, int numFrames)
{
    // ~-74 dB — inaudible, enough that WASAPI does not suspend the stream
    constexpr float amp = 8.0e-5f;

    for (int i = 0; i < numFrames; ++i)
    {
        _keepAliveRng = _keepAliveRng * 1664525u + 1013904223u;
        const float r1 = (float)(_keepAliveRng >> 8) * (1.0f / 16777216.0f);
        _keepAliveRng = _keepAliveRng * 1664525u + 1013904223u;
        const float r2 = (float)(_keepAliveRng >> 8) * (1.0f / 16777216.0f);
        const float x = amp * (r1 + r2 - 1.0f);

        outL[i] += x;
        outR[i] += x;
    }
}

void Engine::setWindowSize(int width, int height)
{
    width = jmax(640, width);
    height = jmax(480, height);

    if (width == _windowWidth && height == _windowHeight)
        return;

    _windowWidth = width;
    _windowHeight = height;
    requestSaveOrganState();
}

AEOLUS_NAMESPACE_END

AEOLUS_NAMESPACE_BEGIN
void Engine::requestProgram(int index) noexcept
{
    _requestedProgram.store(index);
    if (_owningProcessor && getAudioState() == 0) {
        const juce::ScopedLock callbackLock(_owningProcessor->getCallbackLock());
        if (getAudioState() == 0) applyPendingControlsForSetup();
    }
}

bool Engine::postUIControl(ControlAction action, int division, int index, int value)
{
    jassert(juce::MessageManager::getInstance()->isThisTheMessageThread());
    if (_controlEvents.send({action, division, index, value, _layoutEpoch.load()})) {
        if (_owningProcessor && getAudioState() == 0) {
            const juce::ScopedLock callbackLock(_owningProcessor->getCallbackLock());
            if (getAudioState() == 0) applyPendingControlsForSetup();
        }
        return true;
    }
    _rejectedControls.fetch_add(1);
    aeolus::reportUserError(
        "Control queue full", "This control change was not applied. Please try again.");
    return false;
}
void Engine::processControlEvents(int budget)
{
    std::optional<StateWrite> write;
    ControlEvent event;
    for (int count = 0; count < budget && _controlEvents.receive(event); ++count) {
        if (event.epoch != _layoutEpoch.load()) { _rejectedControls.fetch_add(1); continue; }
        if (!write) write.emplace(*this);
        auto* d = juce::isPositiveAndBelow(event.division, getDivisionCount()) ? _divisions[event.division] : nullptr;
        switch (event.action) {
            case ControlAction::Stop: if (d) d->enableStop(event.index, event.value != 0); break;
            case ControlAction::Link: if (d) d->enableLink(event.index, event.value != 0); break;
            case ControlAction::Tremulant: if (d) d->setTremulantEnabled(event.value != 0); break;
            case ControlAction::Bass: if (d) d->setBassCouplerEnabled(event.value != 0); break;
            case ControlAction::CapturePreset: if (d) d->capturePreset(event.index); break;
            case ControlAction::RecallPreset: if (d) d->recallPreset(event.index); break;
            case ControlAction::DisableStops: if (d) d->disableAllStops(); break;
            case ControlAction::CancelStops:
                if (d) { d->disableAllStops(); d->setBassCouplerEnabled(false); } break;
            case ControlAction::GeneralCancel:
                for (auto* division : _divisions) {
                    division->disableAllStops(); division->setBassCouplerEnabled(false);
                    division->cancelAllLinks(); division->setTremulantEnabled(false);
                }
                break;
            case ControlAction::CancelLinks: if (d) d->cancelAllLinks(); break;
            case ControlAction::DivisionMask: if (d) d->setMIDIChannelsMask(event.value); break;
            case ControlAction::ControlMask: setMIDIControlChannelsMask(event.value); break;
            case ControlAction::SwellMask: setMIDISwellChannelsMask(event.value); break;
            case ControlAction::Step: _sequencer->setStep(event.index, event.value != 0); break;
            case ControlAction::CaptureStep: _sequencer->captureStateToStep(event.index); break;
            case ControlAction::Backward: _sequencer->stepBackward(); break;
            case ControlAction::Forward: _sequencer->stepForward(); break;
        }
    }
    const int program = _requestedProgram.exchange(-1);
    if (program >= 0) { if (!write) write.emplace(*this); _sequencer->setStep(program); }
}
AEOLUS_NAMESPACE_END
