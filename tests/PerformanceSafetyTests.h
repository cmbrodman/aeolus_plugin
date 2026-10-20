// Copyright (C) 2026 Aeolus fork contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/PerformanceSettingsComponent.h"

namespace {
void performanceMeterTests()
{
    aeolus::PerformanceMeter m;
    m.record(0, 20000000, 48000, 480, true, 10);
    auto s = m.snapshot();
    check(s.latest == 2 && s.peak == 2 && s.deadlineMisses == 1 && s.recentOverload,
          "meter capped overload or lost missed deadline");
    m.record(1000000000, 1000000, 48000, 480, true, 12);
    s = m.snapshot();
    check(s.peak == 2 && s.xruns == 2 && s.deadlineMisses == 1, "meter peak/counters not retained");
    m.record(3000000000, 1000000, 48000, 480, true, 12);
    s = m.snapshot();
    check(s.peak == .1f && !s.recentOverload, "recent overload/peak did not expire");
    m.record(4000000000, 20000000, 48000, 480, false, -1);
    check(m.snapshot().deadlineMisses == 1 && m.snapshot().xruns == -1, "offline render recorded an xrun/deadline miss");
    m.requestReset(); m.record(5000000000, 1000000, 44100, 441, true, 12);
    check(m.snapshot().deadlineMisses == 0 && m.snapshot().peak == .1f && m.snapshot().xruns == 0,
          "meter reset failed");
    m.record(5500000000, 1000000, 44100, 441, true, 14);
    check(m.snapshot().xruns == 2 && m.snapshot().recentOverload && m.snapshot().deadlineMisses == 0,
          "native xruns not retained/indicated separately from processing misses");
    m.record(5600000000, 1000000, 44100, 441, true, 0);
    check(m.snapshot().xruns == 2, "device counter reset erased retained xruns");
    m.record(6000000000, 1000000, 0, 0, true);
    check(std::isfinite(m.snapshot().average) && m.snapshot().deadlineMisses == 0,
          "zero-length callback corrupted meter");
}

struct VoiceLimitFixture {
    AeolusAudioProcessor processor;
    aeolus::Engine& engine = processor.getEngine();
    juce::AudioBuffer<float> audio{aeolus::N_OUTPUT_CHANNELS, 512};
    juce::MidiBuffer midi;
    VoiceLimitFixture(float rate) {
        processor.setNonRealtime(true);
        for (int d = 0; d < engine.getDivisionCount(); ++d) {
            auto* division = engine.getDivisionByIndex(d);
            division->disableAllStops(); division->cancelAllLinks();
            division->setBassCouplerEnabled(false); division->setMIDIChannelsMask(1 << d);
        }
        processor.prepareToPlay(rate, 512);
        processor.getParametersContainer().reverbWet->setValueNotifyingHost(0);
        midi.ensureSize(2048);
        // JUCE listener bookkeeping allocates on the first input event.
        engine.getMidiKeyboardState().noteOn(16, 60, 1.0f);
        engine.getMidiKeyboardState().noteOff(16, 60, 0.0f);
        engine.resetAdmissionCounters();
    }
    void block() {
        allHeapOperations = 0; countAllHeap = true;
        processor.processBlock(audio, midi);
        countAllHeap = false;
        check(allHeapOperations == 0, "voice admission allocated/freed in callback or workers");
        midi.clear();
    }
    void on(int note = 60, int channel = 1, int offset = 17) {
        midi.addEvent(juce::MidiMessage::noteOn(channel, note, 1.0f), offset); block();
    }
    void off(int note = 60, int channel = 1) {
        midi.addEvent(juce::MidiMessage::noteOff(channel, note), 101); block();
    }
    void finishTails() {
        for (int i = 0; i < 2000 && engine.getVoiceCount() != 0; ++i) block();
        check(engine.getVoiceCount() == 0, "capped voices/tails did not finish");
    }
    int link(int source, int target, int shift = 0, bool passThrough = true) {
        auto* d = engine.getDivisionByIndex(source);
        for (int i = 0; i < d->getLinksCount(); ++i)
            if (d->getLinkByIndex(i).division == engine.getDivisionByIndex(target)) {
                d->getLinkByIndex(i).octaveShift = shift; d->getLinkByIndex(i).passThrough = passThrough; d->enableLink(i, true); return i;
            }
        throw std::runtime_error("fixture needs a coupler");
    }
};

void voiceAdmissionTests()
{
    auto* g = aeolus::EngineGlobal::getInstance();
    const int saved = g->getVoiceLimitSetting();
    for (const auto& env : {std::map<std::string,std::string>{{"AEOLUS_MAX_VOICES","16"}},
                           std::map<std::string,std::string>{{"AEOLUS_MAX_VOICES","16"},{"AEOLUS_DSP_WORKERS","3"}}}) {
        ScopedEnvironment scoped(env);
        for (float rate : {44100.0f, 48000.0f}) {
            g->setVoiceLimitSetting(0);
            {
                VoiceLimitFixture f(rate); auto* d = f.engine.getDivisionByIndex(0);
                int mixture = -1, ranks = 0;
                for (int i = 0; i < d->getStopsCount(); ++i)
                    for (const auto& z : d->getStopByIndex(i).getZones())
                        if (z.isForKey(60) && z.rankwaves.size() > 1) { mixture = i; ranks = int(z.rankwaves.size()); break; }
                check(mixture >= 0 && ranks > 1, "fixture needs a mixture"); d->enableStop(mixture,true);
                g->setVoiceLimitSetting(ranks-1); f.on();
                check(f.engine.getVoiceCount() == 0 && f.engine.getRejectedNoteCount() == 1, "partial mixture admitted");
                g->setVoiceLimitSetting(ranks); for(int i=0;i<5;++i) f.block();
                check(f.engine.getVoiceCount() == 0, "rejected note sounded after capacity increased");
                f.off(); f.on(); check(f.engine.getVoiceCount() == ranks, "complete mixture not admitted");
                f.on(); check(f.engine.getVoiceCount() == ranks && f.engine.getRejectedNoteCount() == 1,
                              "repeated held key duplicated/rejected existing voices");
                g->setVoiceLimitSetting(ranks-1); f.block();
                check(f.engine.getVoiceCount() == ranks, "lowering cap stole existing voices");
                f.on(62); check(f.engine.getVoiceCount() == ranks && f.engine.getRejectedNoteCount() == 2,
                               "new note admitted above lowered limit");
                f.off(); check(f.engine.getVoiceCount() > 0, "note-off truncated release tails");
                f.finishTails(); g->setVoiceLimitSetting(16); f.block();
                check(f.engine.getVoiceCount() == 0, "rejected key replayed after tails freed space");
                f.off(62); f.on(62); check(f.engine.getVoiceCount() == ranks, "new press after rejection failed");
                f.processor.panic(); f.block(); check(f.engine.getVoiceCount() == 0, "Panic failed at voice cap");
            }
            {
                VoiceLimitFixture f(rate); auto* d = f.engine.getDivisionByIndex(0);
                d->enableStop(0,true); g->setVoiceLimitSetting(1); f.on();
                check(f.engine.getVoiceCount() == 1, "single pipe fixture failed");
                d->enableStop(1,true); d->enableStop(2,true); f.block();
                check(f.engine.getVoiceCount() == 1 && f.engine.getRejectedRegistrationCount() == 1,
                      "held-note registration addition partially admitted");
                g->setVoiceLimitSetting(3); for(int i=0;i<5;++i) f.block();
                check(f.engine.getVoiceCount() == 1, "rejected registration queued itself");
                f.off(); f.finishTails(); f.on();
                check(f.engine.getVoiceCount() == 3, "replayed registration did not sound completely");
                f.off(); f.finishTails();
                d->enableStop(1,false); d->enableStop(2,false); f.on();
                d->enableStop(1,true); d->enableStop(2,true); f.block();
                check(f.engine.getVoiceCount() == 3, "enabled stops did not join held note when capacity available");
                f.off(); f.finishTails();
            }
            {
                VoiceLimitFixture f(rate);
                for(int d=0;d<3;++d) f.engine.getDivisionByIndex(d)->enableStop(0,true);
                f.link(2,1); f.link(1,0);
                g->setVoiceLimitSetting(2); f.on(60,3);
                check(f.engine.getVoiceCount() == 0 && f.engine.getRejectedNoteCount() == 1,
                      "transitive coupler admitted incomplete note");
                g->setVoiceLimitSetting(3); f.off(60,3); f.on(60,3); for(int i=0;i<6;++i) f.block();
                check(f.engine.getVoiceCount() == 3, "complete coupled note not admitted");
                auto* target = f.engine.getDivisionByIndex(0); target->enableStop(1,true); f.block();
                check(f.engine.getRejectedRegistrationCount()==1,"coupled registration did not reject at cap");
                g->setVoiceLimitSetting(7); // three release tails plus four new pipe voices
                f.midi.addEvent(juce::MidiMessage::noteOff(3,60),17);
                f.midi.addEvent(juce::MidiMessage::noteOn(3,60,1.0f),18); f.block();
                for(int i=0;i<6;++i) f.block();
                bool addition=false;
                for(auto* v=target->getActiveVoices().first();v;v=v->next())
                    addition |= v->isActive() && v->stopIndex()==1 && v->isForNote(60);
                check(addition,"same-block release/repress left transitive registration blocked");
                f.off(60,3); f.finishTails();
            }
            {
                VoiceLimitFixture f(rate);
                f.engine.getDivisionByIndex(1)->enableStop(0,true);
                f.engine.getDivisionByIndex(0)->enableStop(0,true); f.link(1,0,12);
                g->setVoiceLimitSetting(1); f.on(60,2);
                check(f.engine.getVoiceCount() == 0, "octave-coupled note partially admitted");
                g->setVoiceLimitSetting(2); f.off(60,2); f.on(60,2);
                check(f.engine.getVoiceCount() == 2, "octave-coupled note not admitted");
                f.off(60,2); f.finishTails();
            }
            {
                VoiceLimitFixture f(rate); auto* manual = f.engine.getDivisionByIndex(1);
                manual->enableStop(0,true); auto* pedal = f.engine.getPedalDivision();
                check(pedal != nullptr, "fixture needs pedal division"); pedal->enableStop(0,true);
                manual->setBassCouplerEnabled(true);
                g->setVoiceLimitSetting(1); f.on(60,2); check(f.engine.getVoiceCount() == 0, "bass coupler partial admission");
                g->setVoiceLimitSetting(3); f.off(60,2); f.on(60,2);
                check(f.engine.getVoiceCount() == 2, "bass coupler failed");
                f.on(48,2); check(f.engine.getVoiceCount() == 2 && f.engine.getRejectedNoteCount() == 2,
                                 "bass replacement stole/released voices on failed admission");
                for(auto* v=pedal->getActiveVoices().first();v;v=v->next())
                    check(v->isActive() && v->isForNote(60), "rejected bass note changed sounding bass");
                f.off(48,2); f.off(60,2); f.finishTails();
            }
        }
    }
    g->setVoiceLimitSetting(saved);
}

void saveSafetyScreenshot(juce::Component& component, const juce::String& name)
{
    if (!std::getenv("AEOLUS_TEST_SCREENSHOTS")) return;
    auto folder = aeolus::getTestDataDirectory().getChildFile("screenshots"); folder.createDirectory();
    auto file = folder.getChildFile(name+".png"); file.deleteFile();
    auto stream = file.createOutputStream(); juce::PNGImageFormat png;
    check(stream && png.writeImageToStream(component.createComponentSnapshot(component.getLocalBounds()), *stream),
          "performance screenshot could not be saved");
}

void performanceLayoutTests(AeolusAudioProcessor& processor)
{
    std::unique_ptr<juce::AudioProcessorEditor> editor(processor.createEditor()); editor->setVisible(true);
    for (const int width : {640,1420}) {
        editor->setSize(width,480);
        dynamic_cast<AeolusAudioProcessorEditor*>(editor.get())->timerCallback();
        juce::Component* audio = nullptr; juce::Component* voices = nullptr; juce::Component* settings = nullptr;
        for(auto* child:editor->getChildren()) {
            if(child->getName()=="Audio processing load") audio=child;
            if(child->getName()=="Pipe voice capacity") voices=child;
            if(child->getName()=="settingsButton") settings=child;
        }
        check(audio && voices && settings,"performance meter/menu missing");
        check(editor->getLocalBounds().contains(settings->getBounds()) && !audio->getBounds().intersects(voices->getBounds()),
              "compact performance menu/meters clipped or overlapping");
        for(auto* child:editor->getChildren())
            if(auto* viewport=dynamic_cast<juce::Viewport*>(child))
                check(viewport->getY()>=audio->getBottom(),"division viewport overlaps performance meters");
        saveSafetyScreenshot(*editor,"console-"+juce::String(width));
    }
}

void performanceSettingsTests()
{
    auto* g = aeolus::EngineGlobal::getInstance();
    const int oldLimit = g->getVoiceLimitSetting(), oldWarning = g->getMeterWarningPercent(), oldCritical = g->getMeterCriticalPercent();
    g->setVoiceLimitSetting(123); g->setMeterWarningPercent(77); g->setMeterCriticalPercent(94); g->saveSettings();
    g->setVoiceLimitSetting(321); g->setMeterWarningPercent(60); g->setMeterCriticalPercent(100); g->loadSettings();
    check(g->getVoiceLimitSetting() == 123 && g->getMeterWarningPercent() == 77 && g->getMeterCriticalPercent() == 94,
          "performance preferences failed save/load round trip");
    AeolusAudioProcessor processor;
    performanceLayoutTests(processor);
    using Page = ui::PerformanceSettingsComponent::Page;
    for (const auto page : {Page::VoiceCapacity, Page::Meters, Page::Diagnostics}) {
        ui::PerformanceSettingsComponent panel(processor,page); panel.setSize(440,345);
        saveSafetyScreenshot(panel, "settings-"+juce::String(int(page)));
        juce::TextButton* cancel = nullptr; juce::TextButton* apply = nullptr;
        for (auto* child : panel.getChildren()) {
            if (auto* slider = dynamic_cast<juce::Slider*>(child)) slider->setValue(80);
            if (auto* button = dynamic_cast<juce::TextButton*>(child)) {
                if(button->getButtonText()=="Cancel" || button->getButtonText()=="Close") cancel=button;
                if(button->getButtonText()=="Apply") apply=button;
            }
            if(child->isVisible()) check(panel.getLocalBounds().contains(child->getBounds()), "performance control outside panel");
        }
        check(cancel != nullptr,"performance panel has no cancel/close");
        bool closed=false; panel.onClose=[&]{closed=true;}; cancel->triggerClick(); pump(15);
        check(closed && g->getVoiceLimitSetting()==123 && g->getMeterWarningPercent()==77,
              "cancel changed persisted performance preferences");
        if(page==Page::VoiceCapacity) {
            check(apply!=nullptr,"voice capacity cannot apply"); apply->triggerClick(); pump(15);
            check(g->getVoiceLimitSetting()==80,"voice cap did not apply");
            g->setVoiceLimitSetting(123); g->saveSettings();
        }
        if(page==Page::Meters) {
            check(apply!=nullptr,"meter settings cannot apply"); apply->triggerClick(); pump(15);
            check(g->getMeterWarningPercent()==80 && g->getMeterCriticalPercent()==90,"meter thresholds did not apply/validate");
            g->setMeterWarningPercent(77); g->setMeterCriticalPercent(94);
        }
    }
    {
        auto temporaryProcessor = std::make_unique<AeolusAudioProcessor>();
        auto owner = std::make_unique<juce::Component>();
        auto panel = std::make_unique<ui::PerformanceSettingsComponent>(*temporaryProcessor,Page::Diagnostics,owner.get());
        bool dismissed=false; panel->onClose=[&]{dismissed=true;}; owner.reset();
        check(dismissed,"owner deletion did not dismiss performance diagnostics");
        temporaryProcessor.reset(); pump(230); // The panel timer must no longer read its former processor.
        for(auto* child:panel->getChildren())
            if(auto* button=dynamic_cast<juce::TextButton*>(child);button && button->getButtonText()=="Reset counters") button->triggerClick();
        pump(15); panel.reset();
    }
    g->setVoiceLimitSetting(-10); check(g->getVoiceLimitSetting()==0,"negative cap not validated");
    g->setVoiceLimitSetting(9000); check(g->getVoiceLimitSetting()==4096,"oversized cap not validated");
    check(processor.getEngine().getVoiceLimit()<=processor.getEngine().getVoicePool().getMaxVoices(), "saved cap exceeded startup pool");
    g->setVoiceLimitSetting(oldLimit); g->setMeterWarningPercent(oldWarning); g->setMeterCriticalPercent(oldCritical); g->saveSettings();
}
}
