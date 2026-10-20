// Copyright (C) 2026 Aeolus fork contributors
// SPDX-License-Identifier: GPL-3.0-or-later
namespace {
void resetMemoryFixture()
{
    const auto dir = aeolus::getTestDataDirectory();
    dir.getChildFile("organ_config.json").replaceWithData(BinaryData::default_organ_json, BinaryData::default_organ_jsonSize);
    dir.getChildFile("organ_state.json").replaceWithText("{}");
}

std::vector<float> editTailRender(float rate, bool edit)
{
    resetMemoryFixture();
    VoiceLimitFixture f(rate);
    auto* d = f.engine.getDivisionByIndex(0);
    d->enableStop(0, true);
    f.on();
    for (int i = 0; i < 12; ++i) f.block();
    check(f.engine.getVoiceCount() == 1, "tail fixture requires one voice");
    if (edit) {
        juce::StringArray names;
        for (int i = 0; i < f.engine.getDivisionCount(); ++i) names.add(f.engine.getDivisionByIndex(i)->getName());
        f.processor.suspendForEdit(true);
        f.engine.applyDivisionLayout(f.engine.getDivisionCount(), names);
        f.processor.getParametersContainer().rebindDivisionGains();
        f.processor.suspendForEdit(false);
        check(f.engine.getVoiceCount() == 1, "edit discarded the audible tail");
    } else f.engine.allNotesOff();
    std::vector<float> output;
    for (int i = 0; i < 350; ++i) {
        f.block();
        for (int ch = 0; ch < f.audio.getNumChannels(); ++ch)
            output.insert(output.end(), f.audio.getReadPointer(ch), f.audio.getReadPointer(ch) + f.audio.getNumSamples());
    }
    check(f.engine.getVoiceCount() == 0, "edit stranded voices outside the pool");
    f.on();
    check(f.engine.getVoiceCount() == 1, "edited organ cannot reuse reclaimed capacity");
    f.engine.requestEmergencyPanic(); f.block();
    check(f.engine.getVoiceCount() == 0, "panic left edited voices alive");
    return output;
}

void memoryLifetimeTests()
{
    auto* g = aeolus::EngineGlobal::getInstance();
    g->setVoiceLimitSetting(0);
    for (int workers : {-1, 3}) {
        ScopedEnvironment env(workers < 0 ? std::map<std::string,std::string>{{"AEOLUS_PER_VOICE_RANDOM", "1"}}
            : std::map<std::string,std::string>{{"AEOLUS_DSP_WORKERS", "3"}, {"AEOLUS_PER_VOICE_RANDOM", "1"}});
        for (float rate : {44100.0f, 48000.0f}) {
            const auto reference = editTailRender(rate, false);
            const auto edited = editTailRender(rate, true);
            check(reference.size() == edited.size(), "tail render length changed");
            float difference = 0;
            for (size_t i = 0; i < reference.size(); ++i)
                difference = std::max(difference, std::abs(reference[i] - edited[i]));
            if (difference >= 1e-5f) std::cerr << "Tail difference " << difference << " workers " << workers << " rate " << rate << "\n";
            check(difference < 1e-5f, "layout edit changed or truncated the reference release tail");
        }
    }
    resetMemoryFixture();
    {
        VoiceLimitFixture f(48000);
        f.engine.setMIDIControlChannelsMask(1);
        auto* division = f.engine.getDivisionByIndex(0);
        for (int mode : {0x50, 0x60, 0x70}) {
            f.engine.processMIDIMessage(juce::MidiMessage::controllerEvent(1, 98, mode));
            for (int button = division->getStopsCount(); button < 32; ++button)
                f.engine.processMIDIMessage(juce::MidiMessage::controllerEvent(1, 98, button));
        }
        for (int i = 0; i < division->getStopsCount(); ++i)
            check(!division->isStopEnabled(i), "invalid stop command modified registration");
        f.engine.processMIDIMessage(juce::MidiMessage::controllerEvent(1, 98, 0x60));
        f.engine.processMIDIMessage(juce::MidiMessage::controllerEvent(1, 98, 0));
        check(division->isStopEnabled(0), "valid stop command rejected");
        f.on();
        f.processor.suspendForEdit(true);
        f.engine.applyDivisionLayout(10, {});
        f.processor.getParametersContainer().rebindDivisionGains();
        f.processor.suspendForEdit(false);
        check(f.engine.getDivisionCount() == 10, "division expansion failed");
        check(juce::JSON::parse(aeolus::getOrganStateFile().loadFileAsString()).getProperty("divisions",{}).size()==10,
              "layout migration waited for debounced state save");
        check(f.engine.getDivisionByIndex(0)->getMIDIChannelsMask()==1
              && f.engine.getDivisionByIndex(0)->isStopEnabled(0),
              "division expansion discarded the retained registration or MIDI mask");
        f.finishTails();
        auto pipe = g->getStop(0)->getStopName();
        juce::StringArray pipes;
        for (int i = 0; i < 30; ++i) pipes.add(pipe);
        f.processor.suspendForEdit(true);
        f.engine.applyDivisionStops(0, pipes);
        aeolus::Engine::CouplerEdits ce;
        ce.sourceIndex = 0; ce.hasBassCoupler = false;
        aeolus::Division::LinkSpec spec;
        spec.targetName = f.engine.getDivisionByIndex(1)->getName();
        spec.octaveShift = 0; spec.passThrough = false;
        ce.specs.push_back(spec);
        juce::Array<aeolus::Engine::CouplerEdits> edits; edits.add(ce);
        f.engine.applyCouplerLayouts(edits);
        f.processor.getParametersContainer().rebindDivisionGains();
        f.processor.suspendForEdit(false);
        check(f.engine.getDivisionByIndex(0)->getStopsCount() == 30, "stop expansion failed");
        check(f.engine.getDivisionByIndex(0)->getLinksCount() == 1, "coupler expansion failed");
        for (int step = 0; step < f.engine.getSequencer()->getStepsCount(); ++step)
            f.engine.getSequencer()->setStep(step, false);
        f.processor.suspendForEdit(true);
        f.engine.applyDivisionLayout(1, {});
        f.processor.getParametersContainer().rebindDivisionGains();
        f.processor.suspendForEdit(false);
        f.block();
    }
    resetMemoryFixture();
    {
        // Isolated rank exercises repeated publication and old-state ownership
        // while another thread triggers and plays newly published generations.
        aeolus::Addsynth model;
        auto resource = g->getStop(0);
        const int noteNumber = juce::jlimit(resource->getNoteMin(), resource->getNoteMax(), 69);
        auto state = resource->trigger(noteNumber);
        model.fromVar(state.pipewave->getModel().toVar());
        const auto liveBefore = aeolus::Pipewave::testLivePipeCount.load();
        aeolus::Rankwave rank(model);
        aeolus::Scale scale(aeolus::Scale::EqualTemp);
        rank.createPipes(scale, 440); rank.prepareToPlay(44100);
        auto held = rank.trigger(noteNumber);
        const auto oldFrequency = held.pipewave->getFreqency();
        std::atomic<bool> running{true};
        std::thread reader([&] {
            while (running.load()) {
                auto note = rank.trigger(noteNumber);
                float samples[aeolus::SUB_FRAME_LENGTH]{};
                juce::Random random(123);
                if (note.isTriggered()) note.pipewave->play(note, samples, random);
            }
        });
        for (int i = 0; i < 4; ++i) {
            rank.retunePipes(scale, 450 + i); rank.prepareToPlay(44100);
            rank.collectRetired();
            check(held.pipewave->getFreqency() == oldFrequency, "retune modified a held pipe");
            float samples[aeolus::SUB_FRAME_LENGTH]{};
            juce::Random random(123); held.pipewave->play(held, samples, random);
        }
        running.store(false); reader.join(); rank.collectRetired();
        check(aeolus::Pipewave::testLivePipeCount.load() == liveBefore + unsigned(model.getNoteMax()-model.getNoteMin()+1) + 1,
              "one held note retained whole old rank generations");
        auto latest = rank.trigger(noteNumber);
        const float frequency = latest.pipewave->getFreqency();
        const auto buildCount = aeolus::Pipewave::testWaveformBuildCount.load();
        rank.retunePipes(scale, 453); rank.prepareToPlay(44100);
        check(aeolus::Pipewave::testWaveformBuildCount.load() == buildCount,
              "unchanged tuning rebuilt pipe waveforms");
        auto unchanged = rank.trigger(noteNumber);
        check(unchanged.pipewave == latest.pipewave, "unchanged tuning copied rather than shared its pipe");
        juce::Random originalRandom(123), unchangedRandom(123);
        for (int i = 0; i < 100; ++i) {
            float originalSamples[aeolus::SUB_FRAME_LENGTH]{};
            float unchangedSamples[aeolus::SUB_FRAME_LENGTH]{};
            latest.pipewave->play(latest, originalSamples, originalRandom);
            unchanged.pipewave->play(unchanged, unchangedSamples, unchangedRandom);
            for (int j = 0; j < aeolus::SUB_FRAME_LENGTH; ++j)
                check(originalSamples[j] == unchangedSamples[j], "unchanged tuning regenerated the waveform");
        }
        rank.prepareToPlay(44100);
        check(rank.trigger(noteNumber).pipewave->getFreqency() == frequency, "device restart reverted tuning");
        held.release();
        for (int i = 0; i < 5000 && !held.isOver(); ++i) {
            float samples[aeolus::SUB_FRAME_LENGTH]{};
            juce::Random random(123); held.pipewave->play(held, samples, random);
        }
        check(held.isOver(), "retuned pipe lost its release envelope");
        held.reset(); rank.collectRetired();
        auto beforeRateChange = rank.trigger(noteNumber);
        rank.retunePipes(scale,453); rank.prepareToPlay(48000);
        check(rank.trigger(noteNumber).pipewave != beforeRateChange.pipewave,
              "rate change rebuilt a shared live pipe");
    }
    resetMemoryFixture();
}
}
