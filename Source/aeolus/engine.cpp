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

#include "aeolus/engine.h"
#include <vector>

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

    saveSettings();
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
    }
}

void EngineGlobal::saveSettings()
{
    if (auto* propertiesFile = _globalProperties.getUserSettings()) {
        propertiesFile->setValue(settings::tuningFrequency, _tuningFrequency);
        propertiesFile->setValue(settings::tuningTemperament, (int)_scale.getType());
        propertiesFile->setValue(settings::mtsEnabled, _mtsEnabled);
        propertiesFile->setValue(settings::uiScalingFactor, _uiScalingFactor);
        propertiesFile->setValue(settings::keyboardVisible, _keyboardVisible);
    }

    _globalProperties.saveIfNeeded();
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
    if (_mtsClient == nullptr || !isConnectedToMTSMaster())
    {
        return _scale.getFrequencyForMidoNote(midiNote);
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

    // @note We don't kill active voices - they will be using pipes from a parallel set.
    //       However, switching tuning very fast (while keeping the voice sustained)
    //       may result in voice to be killed.

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

    juce::Array<juce::File> dirs;

    if (true)
    {
        dirs.add(File::getSpecialLocation(File::currentExecutableFile).getParentDirectory());
        dirs.add(File::getSpecialLocation(File::userDocumentsDirectory).getChildFile("Aeolus"));
        dirs.add(File::getSpecialLocation(File::userDocumentsDirectory).getChildFile("Aeolus").getChildFile("Stops"));
    }

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

void EngineGlobal::reloadStopFile(const File& file)
{
    auto* model = Model::getInstance();
    if (model == nullptr || !model->reloadStopFromFile(file))
        return;

    const auto name = file.getFileNameWithoutExtension();

    for (auto* p : _processors)
        p->killAllVoices();

    if (auto* rw = getStopByName(name))
    {
        rw->recreateFromModel(_scale, _tuningFrequency);
        if (_sampleRate > 1.0f)
            rw->prepareToPlay(_sampleRate);
    }
    else if (auto* synth = model->getStopByName(name))
    {
        auto rankwave = std::make_unique<Rankwave>(*synth);
        rankwave->createPipes(_scale, _tuningFrequency);
        if (_sampleRate > 1.0f)
            rankwave->prepareToPlay(_sampleRate);
        auto* ptr = rankwave.get();
        _rankwaves.add(rankwave.release());
        _rankwavesByName.set(ptr->getStopName(), ptr);
    }
}

void EngineGlobal::timerCallback()
{
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
    , _voicePool(*this)
    , _params{NUM_PARAMS}
    , _divisions{}
    , _sequencer{}
    , _subFrameBuffer{N_OUTPUT_CHANNELS, SUB_FRAME_LENGTH}
    , _divisionFrameBuffer{N_OUTPUT_CHANNELS, SUB_FRAME_LENGTH}
    , _voiceFrameBuffer{N_VOICE_CHANNELS, SUB_FRAME_LENGTH}
    , _remainedSamples{0}
    , _tremulantBuffer{1, SUB_FRAME_LENGTH}
    , _tremulantPhase{0.0f}
    , _convolver{}
    , _selectedIR{0}
    , _irSwitchEvents{}
    , _reverbTailCounter{0}
    , _interpolator{1.0f, N_OUTPUT_CHANNELS}
    , _midiKeyboardState{}
    , _volumeLevel{}
    , _midiControlChannelsMask{ (1 << 16) - 1 }
    , _midiSwellChannelsMask{ (1 << 16) - 1 }
{
   populateDivisions();

    // Sequencer can be created only after the divisions have been populated.
    _sequencer = std::make_unique<Sequencer>(*this, SEQUENCER_N_STEPS);

    loadOrganState();
}

void Engine::prepareToPlay(float sampleRate, int frameSize)
{
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

    // Select the first IR for reverb by default
    setReverbIR(_selectedIR);
    _convolver.setDryWet(1.0f, 0.25f, true);

    _interpolator.setRatio(SAMPLE_RATE_F / sampleRate); // 44100 / sampleRate
    _interpolator.reset();

    _sampleRate = sampleRate;

    _samplesSinceAudio = 0;
        _keepAliveSamplesLeft = 0;
        _streamNeedsPrime = false;
        _outputFade = 0.0f;
        _startupFadeDone = false;   

// Prime the convolver with silence so the first real note isn't a click
    constexpr int primeSize = 512;
    juce::HeapBlock<float> zeros(primeSize);
    zeros.clear(primeSize);

    for (int i = 0; i < 16; ++i)
        _convolver.process(zeros, zeros, zeros, zeros, primeSize);

    _reverbTailCounter = _convolver.length();
}

void Engine::setReverbIR(int num)
{
    auto* g = EngineGlobal::getInstance();
    const auto& irs = g->getIRs();

    if (num >= 0 && num < irs.size()) {
        const auto& ir = irs[num];
        _convolver.setLength(int(ir.waveform.getNumSamples() / dsp::Convolver::BlockSize + 1) * dsp::Convolver::BlockSize);
        _convolver.prepareToPlay(SAMPLE_RATE_F, SUB_FRAME_LENGTH); // these parameters are irrelevant
        _convolver.setZeroDelay(ir.zeroDelay);
        _convolver.setIR(ir.waveform);

        _reverbTailCounter = _convolver.length();

        _selectedIR = num;
            requestSaveOrganState();
    }
}

void Engine::postReverbIR(int num)
{
    // Anticipate the IR change that will happen later.
    // This is required for the UI to be updated correctly.
    _selectedIR = num;

    _irSwitchEvents.send({num});
}

float Engine::getReverbLengthInSeconds() const
{
    return float(_convolver.length()) * SAMPLE_RATE_R;
}

void Engine::setReverbWet(float v)
{
    _convolver.setDryWet(1.0f, v);
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
    jassert(outL != nullptr);
    jassert(outR != nullptr);

    float* origOutL = outL;
    float* origOutR = outR;
    int origNumFrames = numFrames;

    processPendingIRSwitchEvents();
    processPendingNoteEvents();

    bool wasAudioGenerated = false;

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
            wasAudioGenerated |= processSubFrame();
            jassert(_remainedSamples > 0);
        }
    }

    // When there is no audio generated we let the reverb tail to
    // sound and stop the reverb processing to avoid convolving with silence.
    if (wasAudioGenerated)
        _reverbTailCounter = _convolver.length();
    else
        _reverbTailCounter = jmax(0, _reverbTailCounter - origNumFrames);

    if (_convolver.isAudible()) {
        _convolver.setNonRealtime(isNonRealtime);
        _convolver.process(origOutL, origOutR, origOutL, origOutR, origNumFrames);
    }

    applyVolume(origOutL, origOutR, origNumFrames);

    // Always dither so WASAPI Low Latency never sees digital silence
    injectKeepAlive(origOutL, origOutR, origNumFrames);
            constexpr float amp = 8.0e-5f;

    // Apply limiter
    if (_limiterEnabled) {
        dsp::Limiter::process(_limiterSpec, _limiterState[0], origOutL, origOutL, origNumFrames);
        dsp::Limiter::process(_limiterSpec, _limiterState[1], origOutR, origOutR, origNumFrames);
    }

    _volumeLevel.left.process(origOutL, origNumFrames);
    _volumeLevel.right.process(origOutR, origNumFrames);
}

void Engine::process(AudioBuffer<float>& out, bool isNonRealtime)
{
    ignoreUnused(isNonRealtime);

    const int numChannels = out.getNumChannels();
    int numFrames = out.getNumSamples();

    processPendingIRSwitchEvents();
    processPendingNoteEvents();

    bool wasAudioGenerated = false;

    int outIdx = 0;

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
            wasAudioGenerated |= processSubFrame();
            jassert(_remainedSamples > 0);
        }

    }

    // Multibus processing does not have a convolver FX

    // Global volume across all the buses
    applyVolume(out);

    _volumeLevel.left.process(out);
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
    for (auto* d : _divisions)
        if (isPedalDivision(d))
            return d;
    return nullptr;
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
    allNotesOff();
    ensureOrganDataFiles();
    const auto configFile = getCustomOrganConfigFile();
    if (!configFile.existsAsFile())
        return;

    var config = JSON::parse(configFile.loadFileAsString());
    auto* obj = config.getDynamicObject();
    if (obj == nullptr)
        return;

    auto divisionsVar = obj->getProperty("divisions");
    auto* arr = divisionsVar.getArray();
    if (arr == nullptr)
        return;

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
    configFile.replaceWithText(JSON::toString(config, true));

    const var savedState = getPersistentState();

        _divisions.clear();
        {
            FileInputStream stream(configFile);
            loadDivisionsFromConfig(stream);
        }
        for (auto* division : _divisions)
            division->clearLinkedDivisions();
        for (auto* division : _divisions)
            division->populateLinkedDivisions();

        setPersistentState(savedState);

        for (auto* division : _divisions)
            division->ensurePresets();
        if (_sequencer != nullptr)
            _sequencer->initFromEngine();
        requestSaveOrganState();
}

void Engine::processMIDIMessage(const MidiMessage& message)
{
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

void Engine::noteOn(int note, int midiChannel)
{
    clearDivisionsTriggerFlag();

    bool handled = false;
    if (midi::matchChannelToMask(getMIDIControlChannelsMask(), midiChannel)) {
        if (isKeySwitchBackward(note)) { _sequencer->stepBackward(); handled = true; }
        else if (isKeySwitchForward(note)) { _sequencer->stepForward(); handled = true; }
    }
    if (handled)
        return;

    auto* g = aeolus::EngineGlobal::getInstance();
    if (g->shouldMTSFilterNote(note, midiChannel))
        return;

    const int nDiv = getDivisionCount();
    std::vector<char> skipLocal((size_t)nDiv, 0);

    for (int i = 0; i < nDiv; ++i)
    {
        auto* src = getDivisionByIndex(i);
        if (midiChannel != 0 && !src->isForMIDIChannel(midiChannel))
            continue;

        for (int L = 0; L < src->getLinksCount(); ++L)
        {
            auto& link = src->getLinkByIndex(L);
            if (!link.enabled || link.division == src)
                continue;
            for (int j = 0; j < nDiv; ++j)
                if (getDivisionByIndex(j) == link.division)
                    skipLocal[(size_t)j] = 1;
        }
    }

    for (int i = 0; i < nDiv; ++i)
    {
        if (skipLocal[(size_t)i])
            continue;
        getDivisionByIndex(i)->noteOn(note, midiChannel, false);
    }

    for (int i = 0; i < nDiv; ++i)
    {
        auto* src = getDivisionByIndex(i);
        if (midiChannel != 0 && !src->isForMIDIChannel(midiChannel))
            continue;

        for (int L = 0; L < src->getLinksCount(); ++L)
        {
            auto& link = src->getLinkByIndex(L);
            if (!link.enabled)
                continue;

            int shift = link.octaveShift;
                        if (shift == 1) shift = 12;
                        else if (shift == -1) shift = -12;
                        else if (shift != 12 && shift != -12) shift = 0;

                        const int coupledNote = note + shift;
            if (coupledNote < 0 || coupledNote >= TOTAL_NOTES)
                continue;

            link.division->triggerNoteInternal(coupledNote);
        }
    }
}

void Engine::noteOff(int note, int midiChannel)
{
    clearDivisionsTriggerFlag();

    const int nDiv = getDivisionCount();
    std::vector<char> skipLocal((size_t)nDiv, 0);

    for (int i = 0; i < nDiv; ++i)
    {
        auto* src = getDivisionByIndex(i);
        if (midiChannel != 0 && !src->isForMIDIChannel(midiChannel))
            continue;

        for (int L = 0; L < src->getLinksCount(); ++L)
        {
            auto& link = src->getLinkByIndex(L);
            if (!link.enabled || link.division == src)
                continue;
            for (int j = 0; j < nDiv; ++j)
                if (getDivisionByIndex(j) == link.division)
                    skipLocal[(size_t)j] = 1;
        }
    }

    for (int i = 0; i < nDiv; ++i)
    {
        if (skipLocal[(size_t)i])
            continue;
        getDivisionByIndex(i)->noteOff(note, midiChannel, false);
    }

    for (int i = 0; i < nDiv; ++i)
    {
        auto* src = getDivisionByIndex(i);
        if (midiChannel != 0 && !src->isForMIDIChannel(midiChannel))
            continue;

        for (int L = 0; L < src->getLinksCount(); ++L)
        {
            auto& link = src->getLinkByIndex(L);
            if (!link.enabled)
                continue;

            int shift = link.octaveShift;
                        if (shift == 1) shift = 12;
                        else if (shift == -1) shift = -12;
                        else if (shift != 12 && shift != -12) shift = 0;

                        const int coupledNote = note + shift;
            if (coupledNote < 0 || coupledNote >= TOTAL_NOTES)
                continue;

            link.division->releaseNoteInternal(coupledNote);
        }
    }
}


void Engine::allNotesOff()
{
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

void Engine::setPersistentState(const var& state)
{
    if (const auto* obj = state.getDynamicObject()) {
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
                setMIDIControlChannelsMask(1 << (ch - 1));
        } else {
            setMIDIControlChannelsMask(obj->getProperty("midi_ctrl_channels_mask"));
        }

        if (const auto& v = obj->getProperty("midi_swell_channel"); !v.isVoid()) {
            int ch = (int)v;
            if (ch == 0)
                setMIDISwellChannelsMask((1 << 16) - 1);
            else
                setMIDISwellChannelsMask(1 << (ch - 1));
        } else {
            setMIDISwellChannelsMask(obj->getProperty("midi_swell_channels_mask"));
        }

        // Restore the IR
        int irNum = obj->getProperty("ir");

        if (MessageManager::getInstance()->isThisTheMessageThread())
            postReverbIR(irNum);
        else
            setReverbIR(irNum);

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

    const auto text = stateFile.loadFileAsString();
    if (text.isEmpty())
        return;

    const auto state = JSON::parse(text);
    if (state.isVoid())
        return;

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
    const auto stateFile = getOrganStateFile();
    const auto state = getPersistentState();
    const auto text = JSON::toString(state, true); // pretty-print

    stateFile.replaceWithText(text);
}

void Engine::requestSaveOrganState()
{
    if (_restoringState.load())
        return;

    juce::MessageManager::callAsync([this]()
    {
        if (_restoringState.load())
            return;

        if (_saveDebouncePending.exchange(true))
            return;

        juce::Timer::callAfterDelay(250, [this]()
        {
            _saveDebouncePending.store(false);

            if (!_restoringState.load())
                saveOrganState();
        });
    });
}

void Engine::populateDivisions()
{
    ensureOrganDataFiles();

    const auto configFile = getCustomOrganConfigFile();

    if (configFile.existsAsFile()) {
        FileInputStream stream(configFile);
        loadDivisionsFromConfig(stream);
    } else {
        MemoryInputStream stream(BinaryData::default_organ_json,
                                 BinaryData::default_organ_jsonSize, false);
        loadDivisionsFromConfig(stream);
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
    count = jlimit(1, 10, count);
    allNotesOff();

    ensureOrganDataFiles();
    const auto configFile = getCustomOrganConfigFile();

    var config;
    if (configFile.existsAsFile())
        config = JSON::parse(configFile.loadFileAsString());
    else
        config = JSON::parse(String::fromUTF8(BinaryData::default_organ_json,
                                              BinaryData::default_organ_jsonSize));

    auto* obj = config.getDynamicObject();
    if (obj == nullptr)
        return;

    if (!obj->getProperty("divisions").isArray())
        obj->setProperty("divisions", Array<var>());

    auto* arr = obj->getProperty("divisions").getArray();

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

    configFile.replaceWithText(JSON::toString(config, true));

    const var savedState = getPersistentState();

    _divisions.clear();
    {
        FileInputStream stream(configFile);
        loadDivisionsFromConfig(stream);
    }
    for (auto* division : _divisions)
        division->clearLinkedDivisions();
    for (auto* division : _divisions)
        division->populateLinkedDivisions();

    setPersistentState(savedState);

    for (auto* division : _divisions)
        division->ensurePresets();
    if (_sequencer != nullptr)
        _sequencer->initFromEngine();
    requestSaveOrganState();
}

void Engine::applyDivisionStops(int divisionIndex, const StringArray& pipeNames)
{
    allNotesOff();
    ensureOrganDataFiles();

    const auto configFile = getCustomOrganConfigFile();
    var config;
    if (configFile.existsAsFile())
        config = JSON::parse(configFile.loadFileAsString());
    else
        return;

    auto* obj = config.getDynamicObject();
    if (obj == nullptr)
        return;

    auto divisionsVar = obj->getProperty("divisions");
    auto* arr = divisionsVar.getArray();
    if (arr == nullptr || !isPositiveAndBelow(divisionIndex, arr->size()))
        return;

    auto* divObj = arr->getReference(divisionIndex).getDynamicObject();
    if (divObj == nullptr)
        return;

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
    configFile.replaceWithText(JSON::toString(config, true));

    const var savedState = getPersistentState();

    _divisions.clear();
    {
        FileInputStream stream(configFile);
        loadDivisionsFromConfig(stream);
    }
    for (auto* division : _divisions)
        division->clearLinkedDivisions();
    for (auto* division : _divisions)
        division->populateLinkedDivisions();

    setPersistentState(savedState);

    for (auto* division : _divisions)
        division->ensurePresets();
    if (_sequencer != nullptr)
        _sequencer->initFromEngine();
    requestSaveOrganState();
}

void Engine::applyDivisionStopsList(const Array<DivisionStopEdits>& edits)
{
    for (const auto& e : edits)
        applyDivisionStops(e.divisionIndex, e.pipeNames);
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
}

void Engine::clearDivisionsTriggerFlag()
{
    for (auto* division : _divisions)
        division->clearTriggerFlag();
}

void Engine::postNoteEvent(bool onOff, int note, int midiChannel)
{
    _pendingNoteEvents.send({onOff, note, midiChannel});
}

bool Engine::processSubFrame()
{
    jassert(_subFrameBuffer.getNumChannels() == _divisionFrameBuffer.getNumChannels());
    jassert(_subFrameBuffer.getNumSamples() == _divisionFrameBuffer.getNumSamples());

    generateTremulant();

    _subFrameBuffer.clear();

    bool wasAudioGenerated = false;

    for (auto* division : _divisions) {

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
    }

    _remainedSamples = SUB_FRAME_LENGTH;

    return wasAudioGenerated;
}

void Engine::processPendingNoteEvents()
{
    NoteEvent event;

    while (_pendingNoteEvents.receive(event)) {
        if (event.on)
            noteOn(event.note, event.midiChannel);
        else
            noteOff(event.note, event.midiChannel);
    }
}

void Engine::processPendingIRSwitchEvents()
{
    IRSwithEvent event;
    bool received = false;

    while (_irSwitchEvents.receive(event)) {
        received = true;
    }

    if (received) {
        setReverbIR(event.num);
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

void Engine::applyVolume(AudioBuffer<float>& out)
{
    if (_params[VOLUME].isSmoothing()) {
        for (int i = 0; i < out.getNumSamples(); ++i) {
            const float g = _params[VOLUME].nextValue() * VOLUME_GAIN;

            for (int ch = 0; ch < out.getNumChannels(); ++ch)
                out.getWritePointer(ch)[i] *= g;
        }
    } else {
        const float g = _params[VOLUME].target() * VOLUME_GAIN;
        out.applyGain(g);
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
    _midiControlChannelsMask = mask;
    requestSaveOrganState();
}

void Engine::setMIDISwellChannelsMask(int mask)
{
    _midiSwellChannelsMask = mask;
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
