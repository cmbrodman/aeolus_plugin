// SPDX-License-Identifier: GPL-3.0-or-later
#if JUCE_LINUX
#include <sys/resource.h>
#include <csignal>
#endif
namespace {
#if JUCE_LINUX
int shortWriteProbe(const juce::File& file)
{
    if (!file.replaceWithText("original")) return 1;
    struct rlimit limit;
    if (getrlimit(RLIMIT_FSIZE,&limit)!=0) return 2;
    limit.rlim_cur=64;
    std::signal(SIGXFSZ,SIG_IGN);
    if (setrlimit(RLIMIT_FSIZE,&limit)!=0) return 3;
    const auto result=aeolus::saveTextFile(file,juce::String::repeatedString("x",256));
    const bool preserved=result.failed() && file.loadFileAsString()=="original";
    file.deleteFile();
    return preserved ? 0 : 4;
}
#endif
bool sounding(aeolus::Division& division, int note) {
    for (auto* voice = division.getActiveVoices().first(); voice; voice = voice->next())
        if (voice->isActive() && voice->isForNote(note)) return true;
    return false;
}
void configureAuditLink(int source, int target, int shift = 0)
{
    auto file=aeolus::getCustomOrganConfigFile();
    auto config=juce::JSON::parse(file.loadFileAsString());
    auto* divisions=config.getProperty("divisions",{}).getArray();
    auto* object=(*divisions)[source].getDynamicObject();
    juce::Array<juce::var> links;
    if (auto* existing=object->getProperty("links").getArray()) links=*existing;
    auto* link=new juce::DynamicObject();
    link->setProperty("to",(*divisions)[target].getProperty("name",{}));
    link->setProperty("kind",shift<0 ? "sub" : shift>0 ? "super" : "unison");
    link->setProperty("passthrough",true); links.add(juce::var(link));
    object->setProperty("links",links);
    check(aeolus::saveTextFile(file,juce::JSON::toString(config)).wasOk(),"coupler test configuration write failed");
}
void auditKeyTests()
{
    auto* global = aeolus::EngineGlobal::getInstance();
    global->setVoiceLimitSetting(0);
    resetMemoryFixture();
    {
        VoiceLimitFixture f(44100);
        for (int i=0;i<f.engine.getDivisionCount();++i) {
            auto* d=f.engine.getDivisionByIndex(i);
            for(int stop=0;stop<d->getStopsCount();++stop) d->enableStop(stop,true);
            d->setTremulantEnabled(true); d->setBassCouplerEnabled(true);
        }
        check(f.engine.postUIControl(aeolus::ControlAction::GeneralCancel),"general cancel enqueue failed");
        f.block();
        for(int i=0;i<f.engine.getDivisionCount();++i) {
            auto* d=f.engine.getDivisionByIndex(i);
            for(int stop=0;stop<d->getStopsCount();++stop) check(!d->isStopEnabled(stop),"general cancel split registration across blocks");
            check(!d->isTremulantEnabled() && !d->isBassCouplerEnabled(),"general cancel left a control enabled");
        }
    }
    resetMemoryFixture();
    {
        VoiceLimitFixture f(44100);
        auto* first=f.engine.getDivisionByIndex(0); auto* second=f.engine.getDivisionByIndex(1);
        first->enableStop(0,true); second->enableStop(0,true);
        f.on(60,1); global->setVoiceLimitSetting(2);
        // An oversized registration addition on the old key must not be
        // charged to an unrelated incoming note before the next quantum.
        for(int stop=1;stop<first->getStopsCount();++stop) first->enableStop(stop,true);
        f.engine.noteOn(64,2);
        check(f.engine.getRejectedNoteCount()==0 && sounding(*second,64),"unrelated registration addition rejected incoming note");
        f.block(); f.off(60,1); f.engine.noteOff(64,2); f.block(); f.finishTails();
        global->setVoiceLimitSetting(0);
    }
    resetMemoryFixture(); configureAuditLink(0,1);
    {
        VoiceLimitFixture f(44100);
        auto* old=f.engine.getDivisionByIndex(0); auto* target=f.engine.getDivisionByIndex(1);
        auto* incoming=f.engine.getDivisionByIndex(2);
        old->enableStop(0,true); incoming->enableStop(0,true);
        for(int stop=0;stop<target->getStopsCount();++stop) target->enableStop(stop,true);
        f.on(60,1); global->setVoiceLimitSetting(2);
        f.link(0,1);
        f.engine.noteOn(64,3);
        check(f.engine.getRejectedNoteCount()==0 && sounding(*incoming,64),"stale coupler graph charged held-key addition to incoming note");
        f.engine.noteOff(64,3); f.off(60,1); f.finishTails();
        global->setVoiceLimitSetting(0);
        old->cancelAllLinks(); old->disableAllStops(); old->enableStop(0,true);
        f.on(60,1); global->setVoiceLimitSetting(1); f.engine.allNotesOff();
        f.engine.noteOn(60,1);
        check(f.engine.getRejectedNoteCount()==1,"all-notes-off re-press bypassed note admission");
        f.engine.noteOff(60,1); f.finishTails(); global->setVoiceLimitSetting(0);
    }
    for (int workers : {0, 3}) {
        ScopedEnvironment env({{"AEOLUS_DSP_WORKERS", std::to_string(workers)}, {"AEOLUS_PER_VOICE_RANDOM", "1"}});
        for (float rate : {44100.f, 48000.f}) {
            resetMemoryFixture(); configureAuditLink(0,1);
            {
                VoiceLimitFixture f(rate);
                for (int i=0;i<3;++i) f.engine.getDivisionByIndex(i)->enableStop(0,true);
                f.link(0,1); f.link(1,0);
                f.on(); f.off(); f.finishTails();
                check(f.engine.getVoiceCount()==0,"reciprocal couplers sustained physical release");
            }
            resetMemoryFixture();
            {
                VoiceLimitFixture f(rate);
                auto* d=f.engine.getDivisionByIndex(0); d->enableStop(0,true); d->setMIDIChannelsMask(3);
                f.on(60,1); f.on(60,2); f.off(60,1);
                check(sounding(*d,60),"one channel released another channel's held pitch");
                f.off(60,2); f.finishTails();
                f.on(60,1); d->setMIDIChannelsMask(2); f.block(); f.finishTails();
                f.off(60,1); f.on(60,2); check(sounding(*d,60),"MIDI mask removal stranded key ownership");
                f.off(60,2); f.finishTails();
            }
            resetMemoryFixture();
            {
                VoiceLimitFixture f(rate);
                auto* manual=f.engine.getDivisionByIndex(1); auto* pedal=f.engine.getPedalDivision();
                manual->enableStop(0,true); pedal->enableStop(0,true);
                pedal->setMIDIChannelsMask(4); manual->setBassCouplerEnabled(true);
                f.on(60,2); f.on(60,3); manual->setBassCouplerEnabled(false); f.block();
                check(sounding(*pedal,60),"bass disable released independently held pedal");
                f.off(60,2); f.off(60,3); f.finishTails();
                manual->setBassCouplerEnabled(true); f.on(60,2);
                manual->setBassCouplerEnabled(false); f.off(60,2); f.finishTails();
            }
            resetMemoryFixture();
            {
                VoiceLimitFixture f(rate);
                for(int i=0;i<3;++i) f.engine.getDivisionByIndex(i)->enableStop(0,true);
                f.link(2,1,0,false); f.link(1,0,0,true); f.on(60,3);
                check(!sounding(*f.engine.getDivisionByIndex(0),60),"disabled pass-through propagated note");
                f.off(60,3); f.finishTails();
                f.link(2,1,0,true); f.on(60,3);
                check(sounding(*f.engine.getDivisionByIndex(0),60),"enabled pass-through failed");
                f.off(60,3); f.finishTails();
            }
            resetMemoryFixture(); configureAuditLink(1,1,-12);
            {
                VoiceLimitFixture f(rate); auto* d=f.engine.getDivisionByIndex(1); d->enableStop(0,true);
                f.link(1,1,-12,true); global->setVoiceLimitSetting(2); f.on(60,2);
                check(f.engine.getVoiceCount()==2 && sounding(*d,48),"self-sub coupler over-reserved voices");
                f.off(60,2); f.finishTails(); global->setVoiceLimitSetting(0);
            }
        }
    }
}
void auditDataTests()
{
    int embedded=0;
    for (int i=0;i<BinaryData::namedResourceListSize;++i) {
        const juce::String filename(BinaryData::originalFilenames[i]);
        aeolus::Addsynth stop;
        if (filename.endsWith(".ae0")) {
            check(stop.readFromResource(BinaryData::namedResourceList[i]).wasOk(),"bundled binary stop failed validation"); ++embedded;
        } else if(filename.endsWith("_ae0.json")) {
            int bytes=0; const auto* data=BinaryData::getNamedResource(BinaryData::namedResourceList[i],bytes);
            check(aeolus::Addsynth::validate(juce::JSON::parse(juce::String::fromUTF8(data,bytes))).wasOk(),"bundled JSON stop failed validation"); ++embedded;
        }
    }
    check(embedded==85,"bundled stop validation inventory changed");
    aeolus::N_func table; table.setValue(0,0); table.setValue(10,10); table.clearValue(4); table.setValue(5,20);
    table.clearValue(5); check(!table.isSet(5) && std::abs(table.getValue(5)-5)<1e-6f,"clearValue failed interpolation");
    table.clearValue(-1); table.clearValue(100);
    check(aeolus::midi::channelToMask(-1)==0 && aeolus::midi::channelToMask(33)==0,"invalid MIDI channel shift");
    auto* g=aeolus::EngineGlobal::getInstance();
    auto state=g->getStop(0)->trigger(g->getStop(0)->getNoteMin());
    aeolus::Addsynth model; model.fromVar(state.pipewave->getModel().toVar());
    const auto original=juce::JSON::toString(model.toVar());
    auto file=aeolus::getTestDataDirectory().getChildFile("bad-stop.json");
    for (auto text : {juce::String("{"),juce::String("{}"),juce::String("null")}) {
        file.replaceWithText(text);
        check(model.readFromFile(file).failed() && juce::JSON::toString(model.toVar())==original,"invalid JSON replaced live stop");
    }
    auto legacyRange=model.toVar();
    legacyRange.getDynamicObject()->setProperty("note_min",30);
    legacyRange.getDynamicObject()->setProperty("note_max",46);
    file.replaceWithText(juce::JSON::toString(legacyRange));
    check(model.readFromFile(file).failed() && juce::JSON::toString(model.toVar())==original,
          "legacy note-range conversion bypassed validation");
    auto excessive=model.toVar();
    auto* loudVolume=excessive.getDynamicObject()->getProperty("n_vol").getDynamicObject()->getProperty("values").getArray();
    for (auto& value : *loudVolume) value=40;
    auto* loudHarmonic=excessive.getDynamicObject()->getProperty("h_lev").getArray()->getReference(0).getDynamicObject()->getProperty("values").getArray();
    for (auto& value : *loudHarmonic) value=40;
    check(aeolus::Addsynth::validate(excessive).failed(),"excessive combined harmonic gain accepted");
    auto unnamed=model.toVar(); unnamed.getDynamicObject()->removeProperty("name");
    file.replaceWithText(juce::JSON::toString(unnamed));
    check(model.readFromFile(file).wasOk() && model.getStopName()==file.getFileNameWithoutExtension(),"filename fallback for unnamed stop failed");
    model.fromVar(juce::JSON::parse(original));
    for (auto field : {"fd","note_min","n_harm"}) {
        auto invalid=model.toVar(); invalid.getDynamicObject()->setProperty(field,juce::String(field)=="note_min" ? 300 : 0);
        file.replaceWithText(juce::JSON::toString(invalid));
        check(model.readFromFile(file).failed(),"invalid stop schema accepted");
    }
    auto legacy=model.toVar(); legacy.getDynamicObject()->setProperty("n_harm",48);
    for (const char* key : {"h_lev","h_ran","h_att","h_atp"}) legacy.getDynamicObject()->getProperty(key).getArray()->removeRange(48,16);
    file.replaceWithText(juce::JSON::toString(legacy));
    check(model.readFromFile(file).wasOk() && model.getHarmonicLevel(0,0)==float(legacy.getProperty("h_lev",{})[0].getProperty("values",{})[0]),"legacy harmonic data was discarded");
    model.fromVar(juce::JSON::parse(original));
    juce::MemoryOutputStream binary; model.write(binary);
    for (size_t bytes : {size_t(1),size_t(31),binary.getDataSize()-1}) {
        juce::MemoryInputStream input(binary.getData(),bytes,false);
        check(model.read(input).failed() && juce::JSON::toString(model.toVar())==original,"truncated binary replaced live stop");
    }
    for (float frequency : {220.f,440.f,880.f}) {
        aeolus::Pipewave pipe(model,0,frequency);
        check(pipe.getPipeFrequency()==frequency,"footage ratio applied twice");
    }
    const auto sentinel=aeolus::getTestDataDirectory().getChildFile("save-sentinel.json");
    check(aeolus::saveTextFile(sentinel,"original").wasOk(),"save sentinel initialization failed");
    const auto invalidPath=sentinel.getChildFile("child.json");
    check(aeolus::saveTextFile(invalidPath,"replacement").failed() && sentinel.loadFileAsString()=="original","failed save damaged existing data");
#if JUCE_LINUX
    juce::ChildProcess shortWrite;
    const auto probe=aeolus::getTestDataDirectory().getChildFile("short-write-sentinel.json");
    check(shortWrite.start(juce::StringArray{juce::File::getSpecialLocation(juce::File::currentExecutableFile).getFullPathName(),
              "--short-write-probe",probe.getFullPathName()})
          && shortWrite.waitForProcessToFinish(10000) && shortWrite.getExitCode()==0,
          "short buffered write replaced valid data");
#endif
    file.deleteFile(); sentinel.deleteFile();
    const auto blocker=aeolus::getTestDataDirectory().getChildFile("unwritable-profile-file.json");
    if (blocker.isDirectory()) check(blocker.deleteRecursively(),"old test fixture cleanup failed");
    check(aeolus::saveTextFile(blocker,"not a directory").wasOk(),"profile failure fixture creation failed");
    {
        struct RestoreTestDirectory {
            std::string previous;
            ~RestoreTestDirectory() { setEnvironment("AEOLUS_TEST_DATA_DIRECTORY",previous.c_str()); }
        } restore{juce::SystemStats::getEnvironmentVariable("AEOLUS_TEST_DATA_DIRECTORY",{}).toStdString()};
        setEnvironment("AEOLUS_TEST_DATA_DIRECTORY",blocker.getFullPathName().toRawUTF8());
        check(aeolus::ensureOrganDataFiles().failed() && aeolus::ensureStopFiles().failed(),"failed initialization was not reported");
        AeolusAudioProcessor fallback;
        check(fallback.getEngine().getDivisionCount()>0,"unwritable profile prevented embedded fallback");
        fallback.setCurrentProgram(3);
        check(fallback.getCurrentProgram()==3,"host program inactive with no audio device");
        auto* division=fallback.getEngine().getDivisionByIndex(0);
        division->postControl(aeolus::ControlAction::Stop,0,1);
        check(division->isStopEnabled(0),"UI controls inactive with no audio device");
    }
    check(blocker.loadFileAsString()=="not a directory","fallback damaged existing data");
    blocker.deleteFile();
}
void auditControlTests()
{
    resetMemoryFixture();
    VoiceLimitFixture f(48000); auto* d=f.engine.getDivisionByIndex(0);
    check(d->postControl(aeolus::ControlAction::Stop,0,1),"UI control enqueue rejected");
    check(!d->isStopEnabled(0),"UI mutated audio registration before dispatch");
    f.block(); check(d->isStopEnabled(0),"queued UI registration not applied");
    for(int i=0;i<50;++i) {
        d->postControl(aeolus::ControlAction::CapturePreset,0);
        f.engine.getSequencer()->postControl(aeolus::ControlAction::CaptureStep,0);
        f.on(60,1); d->postControl(aeolus::ControlAction::RecallPreset,0); f.off(60,1);
    }
    f.finishTails();
    auto state=f.engine.getPersistentState(); check(!state.isVoid(),"idle registration snapshot failed");
    {
        ui::MidiSettingsComponent settings(f.engine);
        f.engine.postUIControl(aeolus::ControlAction::ControlMask,-1,0,3); f.block(); pump(150);
        bool refreshed=false;
        for (auto* child : settings.getChildren()) for (auto* grandchild : child->getChildren())
            if (auto* label=dynamic_cast<juce::Label*>(grandchild)) refreshed |= label->getText()=="1,2";
        check(refreshed,"MIDI settings label did not acknowledge queued channel change");
        const auto epoch=f.engine.getLayoutEpoch();
        for(int replacement=0;replacement<2;++replacement) {
            f.processor.suspendForEdit(true);
            f.engine.applyDivisionLayout(f.engine.getDivisionCount(),{});
            f.processor.getParametersContainer().rebindDivisionGains();
            f.processor.suspendForEdit(false);
        }
        check(f.engine.getLayoutEpoch()!=epoch,"settings lifetime fixture did not replace divisions");
        pump(150); // refresh after original and retired divisions were deleted
        d=f.engine.getDivisionByIndex(0);
    }
    const auto stops=aeolus::getOrganDataDirectory().getChildFile("Stops");
    const auto name=d->getStopByIndex(0).getZones()[0].rankwaves[0]->getStopName();
    const auto expected=stops.getChildFile(name+"__32ft.json");
    expected.deleteFile(); // test-owned prior variant only
    const auto before=stops.findChildFiles(juce::File::findFiles,false,"*.json").size();
    { ui::StopEditorComponent editor(f.engine); editor.testStageVariant(name,1); }
    check(stops.findChildFiles(juce::File::findFiles,false,"*.json").size()==before,"cancelled footage edit wrote a variant");
    { ui::StopEditorComponent editor(f.engine); editor.testStageVariant(name,1); editor.testStageVariant(name+"__32ft",4);
      check(editor.commitVariants() && !expected.existsAsFile(),"abandoned footage variant was committed"); }
    { ui::StopEditorComponent editor(f.engine); editor.testStageVariant(name,1); check(editor.commitVariants(),"variant commit failed"); }
    check(expected.existsAsFile(),"committed footage variant missing from test directory");
    check(aeolus::getStopSearchDirectories().contains(expected.getParentDirectory()),"variant directory is not searched on restart");
    f.on();
    auto* global=aeolus::EngineGlobal::getInstance();
    global->reloadStopFile(expected);
    check(global->hasDeferredStopReloads(),"variant rollback fixture did not defer reload");
    check(expected.deleteFile(),"variant rollback fixture could not remove file");
    global->reloadStopFile(expected);
    check(!global->hasDeferredStopReloads(),"removed variant retained obsolete deferred reload");
    f.off(); f.finishTails();
    {
        const auto customFile=stops.getChildFile(name+"__32ft.AE0");
        const auto originalVariant=aeolus::Model::getInstance()->getStopByName(name+"__32ft")->toVar();
        auto customValue=originalVariant.clone();
        for(auto& volume : *customValue.getProperty("n_vol",{}).getDynamicObject()->getProperty("values").getArray()) volume=-100;
        aeolus::Addsynth custom; custom.fromVar(customValue);
        juce::MemoryOutputStream bytes; custom.write(bytes);
        check(customFile.replaceWithData(bytes.getData(),bytes.getDataSize()),"custom variant fixture write failed");
        f.on();
        { ui::StopEditorComponent pending(f.engine); pending.testStageVariant(name,1);
          check(!pending.commitVariants() && !expected.existsAsFile(),"deferred model change committed stale footage"); }
        f.off(); f.finishTails();
        check(global->reloadStopFile(customFile),"custom variant reload did not become ready");
        juce::MemoryBlock originalBytes, afterBytes;
        check(customFile.loadFileAsData(originalBytes),"custom variant fixture read failed");
        auto* rank=global->getStopByName(name+"__32ft");
        auto beforePipe=rank->trigger(rank->getNoteMin());
        check(beforePipe.pipewave->getModel().getNoteVolume(0)==-100,"prepared pipe lost its private model owner");
        ui::StopEditorComponent editor(f.engine); editor.testStageVariant(name,1);
        check(editor.commitVariants() && !expected.existsAsFile()
              && rank->trigger(rank->getNoteMin()).pipewave==beforePipe.pipewave
              && aeolus::Model::getInstance()->getStopByName(name+"__32ft")->getNoteVolume(0)==-100
              && customFile.loadFileAsData(afterBytes) && afterBytes==originalBytes,
              "footage edit shadowed an existing custom variant");
        const bool mtsWasEnabled=global->isMTSEnabled();
        const auto tuningWas=global->getTuningFrequency();
        global->setMTSEnabled(false); global->setTuningFrequency(445);
        check(std::abs(global->getMTSNoteToFrequency(60)-global->getScale().getFrequencyForMidoNote(60,445))<1e-5f,
              "MTS fallback ignored configured local tuning");
        global->setTuningFrequency(tuningWas);
        global->setMTSFrequencyBaseForTest(300);
        global->setMTSEnabled(true);
        check(global->reloadStopFile(customFile),"MTS reload failed");
        for(int note : {rank->getNoteMin(),rank->getNoteMin()+7,rank->getNoteMin()+24}) {
            auto tuned=rank->trigger(note);
            check(tuned.pipewave && std::abs(tuned.pipewave->getPipeFrequency()-(300.f+note*1.25f)*.25f)<1e-5f,
                  "stop reload ignored active per-note MTS tuning");
        }
        global->setMTSFrequencyBaseForTest(0);
        global->setMTSEnabled(mtsWasEnabled);
        customFile.deleteFile();
        check(aeolus::saveTextFile(expected,juce::JSON::toString(originalVariant)).wasOk()
              && global->reloadStopFile(expected),"custom variant cache cleanup failed");
        check(beforePipe.pipewave->getModel().getNoteVolume(0)==-100,"retired pipe metadata changed after model replacement");
        expected.deleteFile();
    }
    const auto configFile=aeolus::getCustomOrganConfigFile(); const auto savedConfig=configFile.loadFileAsString();
    check(aeolus::saveTextFile(configFile,"{invalid").wasOk(),"invalid config fixture creation failed");
    const auto rejected=stops.getChildFile(name+"__16ft.json"); rejected.deleteFile();
    { ui::StopEditorComponent editor(f.engine); editor.testStageVariant(name,2);
      check(!editor.commitVariants() && !rejected.existsAsFile(),"unreadable config left an orphan footage variant"); }
    check(aeolus::saveTextFile(configFile,savedConfig).wasOk(),"invalid config fixture cleanup failed");
    auto shrunk=juce::JSON::parse(savedConfig);
    shrunk.getDynamicObject()->getProperty("divisions").getArray()->removeLast();
    check(aeolus::saveTextFile(configFile,juce::JSON::toString(shrunk)).wasOk(),"short config fixture write failed");
    f.processor.suspendForEdit(true);
    check(!f.engine.applyDivisionStops(f.engine.getDivisionCount()-1,{},true)
          && f.engine.getPendingErrorForTest().contains("missing"),"missing division edit failed silently");
    f.processor.suspendForEdit(false);
    check(aeolus::saveTextFile(configFile,savedConfig).wasOk(),"short config fixture cleanup failed");
    {
        auto processor=std::make_unique<AeolusAudioProcessor>();
        auto owner=std::make_unique<juce::Component>();
        ui::MidiSettingsComponent orphan(processor->getEngine(),nullptr,owner.get());
        owner.reset(); processor.reset();
        pump(150); // panel timer outlives both its editor owner and the engine
        for(auto* child : orphan.getChildren())
            if(auto* selector=dynamic_cast<ui::MidiChannelsComponent*>(child)) selector->updateLabel();
    }


}
void auditConcurrentControls()
{
    resetMemoryFixture();
    VoiceLimitFixture f(48000);
    std::atomic<bool> running{true}, ready{false}, finite{true};
    std::atomic<int> skipped{0};
    std::thread audio([&] {
        juce::AudioBuffer<float> buffer(aeolus::N_OUTPUT_CHANNELS,128);
        juce::MidiBuffer midi; midi.ensureSize(1024);
        ready.store(true);
        int block=0;
        while(running.load()) {
            midi.clear();
            if(block%4==0) midi.addEvent(juce::MidiMessage::noteOn(1,60,1.f),17);
            if(block%4==2) midi.addEvent(juce::MidiMessage::noteOff(1,60),31);
            {
                const juce::ScopedLock callbackLock(f.processor.getCallbackLock());
                if (!f.processor.isSuspended()) f.processor.processBlock(buffer,midi);
                else { skipped.fetch_add(1); buffer.clear(); }
            }
            for(int ch=0;ch<buffer.getNumChannels();++ch) for(int i=0;i<buffer.getNumSamples();++i)
                if(!std::isfinite(buffer.getSample(ch,i))) finite.store(false);
            ++block;
        }
    });
    while(!ready.load()) std::this_thread::yield();
    bool enqueued=true, snapshots=false;
    for(int i=0;i<120;++i) {
        auto* d=f.engine.getDivisionByIndex(0);
        enqueued &= d->postControl(aeolus::ControlAction::Stop,0,i%2);
        enqueued &= d->postControl(aeolus::ControlAction::Bass,0,i%2);
        enqueued &= d->postControl(aeolus::ControlAction::CapturePreset,i%10);
        enqueued &= d->postControl(aeolus::ControlAction::RecallPreset,i%10);
        enqueued &= f.engine.getSequencer()->postControl(aeolus::ControlAction::CaptureStep,i%10);
        enqueued &= f.engine.getSequencer()->postControl(aeolus::ControlAction::Step,i%10,0);
        f.engine.requestProgram(i%10);
        auto state=f.engine.getPersistentState(); snapshots |= !state.isVoid();
        (void)d->presetMatchesCurrent(i%10);
    }
    f.engine.requestProgram(-1);
    enqueued &= f.engine.getDivisionByIndex(0)->postControl(aeolus::ControlAction::Stop,0,1);
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(20);
    while(f.engine.getPendingControlCount() && std::chrono::steady_clock::now()<deadline) std::this_thread::yield();
    const auto finalState=f.engine.getPersistentState();
    snapshots |= !finalState.isVoid();
    const bool fresh=bool(finalState.getProperty("divisions",{})[0].getProperty("stops",{})[0].getProperty("enabled",false));
    juce::MemoryBlock hostState;
    for(int capture=0;capture<12;++capture) f.processor.getStateInformation(hostState);
    const auto hostSnapshot=juce::JSON::parse(juce::String::fromUTF8(static_cast<const char*>(hostState.getData()),int(hostState.getSize())));
    const bool hostFresh=bool(hostSnapshot.getProperty("divisions",{})[0].getProperty("stops",{})[0].getProperty("enabled",false));
    f.processor.setStateInformation(hostState.getData(),int(hostState.getSize()));
    running.store(false); audio.join();
    check(enqueued && snapshots && finite.load() && fresh && hostFresh && skipped.load()==0,
          "host state capture/restore dropped a callback or returned stale registration");
    f.processor.panic(); f.block();
    const auto rejected=f.engine.getRejectedControlCount();
    std::atomic<bool> deviceStarted{false};
    std::thread device([&] {
        deviceStarted.store(true);
        for(int cycle=0;cycle<10;++cycle) {
            f.processor.releaseResources();
            f.processor.prepareToPlay(48000,512);
        }
    });
    while(!deviceStarted.load()) std::this_thread::yield();
    const auto freshModel=aeolus::getTestDataDirectory().getChildFile("Stops").getChildFile("prepare-publication-"+juce::Uuid().toString()+".json");
    auto model=aeolus::Model::getInstance()->getStopByName(f.engine.getDivisionByIndex(0)->getStopByIndex(0).getZones()[0].rankwaves[0]->getStopName())->toVar();
    check(aeolus::saveTextFile(freshModel,juce::JSON::toString(model)).wasOk(),"publication fixture write failed");
    const bool published=aeolus::EngineGlobal::getInstance()->reloadStopFile(freshModel);
    for(int control=0;control<100;++control) {
        f.engine.postUIControl(aeolus::ControlAction::Stop,0,0,control%2);
        f.engine.requestProgram(control%10);
    }
    device.join();
    for(int drain=0;drain<128 && f.engine.getPendingControlCount();++drain) f.block();
    f.block(); // consume a program mailbox even if the event queue was already empty
    f.engine.postUIControl(aeolus::ControlAction::Stop,0,0,1); f.block();
    check(published && aeolus::EngineGlobal::getInstance()->getStopByName(freshModel.getFileNameWithoutExtension())
          && f.engine.getPendingControlCount()==0 && f.engine.isEmergencyMuted()
          && f.engine.getRejectedControlCount()==rejected
          && f.engine.getDivisionByIndex(0)->isStopEnabled(0)
          && f.engine.getSequencer()->getCurrentStep()==9,
          "prepare/control/publication ownership lost state, controls or Panic");
    freshModel.deleteFile();
}

void auditRejectedLayoutTests()
{
    resetMemoryFixture();
    ScopedEnvironment environment({{"AEOLUS_DSP_WORKERS","3"}});
    VoiceLimitFixture f(48000);
    const int count=f.engine.getDivisionCount();
    const auto original=aeolus::getCustomOrganConfigFile().loadFileAsString();
    f.processor.suspendForEdit(true);
    f.engine.applyDivisionLayout(count,{});
    f.processor.getParametersContainer().rebindDivisionGains();
    f.processor.suspendForEdit(false);
    f.engine.getDivisionByIndex(0)->enableStop(0,true); f.on();
    auto config=juce::JSON::parse(aeolus::getCustomOrganConfigFile().loadFileAsString());
    auto* divisions=config.getDynamicObject()->getProperty("divisions").getArray();
    while(divisions->size()<11) divisions->add((*divisions)[0].clone());
    const auto invalid=juce::JSON::toString(config);
    check(aeolus::saveTextFile(aeolus::getCustomOrganConfigFile(),invalid).wasOk(),"invalid-layout fixture write failed");
    const auto writtenInvalid=aeolus::getCustomOrganConfigFile().loadFileAsString();
    f.processor.suspendForEdit(true);
    f.engine.applyCouplerLayout(0,false,{});
    f.processor.suspendForEdit(false);
    check(f.engine.getDivisionCount()==count && f.engine.getVoiceCount()==1,"rejected layout discarded live divisions");
    f.block(); f.off(); f.finishTails();
    check(aeolus::getCustomOrganConfigFile().loadFileAsString()==writtenInvalid,"rejected layout overwrote custom config");
    {
        AeolusAudioProcessor fallback;
        check(fallback.getEngine().getDivisionCount()==count,"unsupported layout did not use embedded fallback");
        fallback.suspendForEdit(true);
        fallback.getEngine().applyDivisionLayout(count,{});
        fallback.suspendForEdit(false);
        check(aeolus::getCustomOrganConfigFile().loadFileAsString()==writtenInvalid
              && fallback.getEngine().getPendingErrorForTest().contains("preserved"),
              "fallback layout edit overwrote rejected custom config");
        fallback.getEngine().saveOrganState();
    }
    check(aeolus::saveTextFile(aeolus::getCustomOrganConfigFile(),original).wasOk(),"layout fixture cleanup failed");
    const auto stateFile=aeolus::getOrganStateFile();
    check(aeolus::saveTextFile(stateFile,"{invalid").wasOk(),"malformed-state fixture creation failed");
    { AeolusAudioProcessor preserved;
      check(preserved.getEngine().getPendingErrorForTest().contains("disabled"),"disabled persistence was not reported");
      preserved.getEngine().saveOrganState(); }
    check(stateFile.loadFileAsString()=="{invalid","failed state load was overwritten");
    check(aeolus::saveTextFile(stateFile,"").wasOk(),"empty-state fixture creation failed");
    { AeolusAudioProcessor empty; empty.getEngine().saveOrganState(); }
    check(juce::JSON::parse(stateFile.loadFileAsString()).isObject(),"empty state prevented future saves");
    check(aeolus::saveTextFile(stateFile,"{}").wasOk(),"state fixture cleanup failed");
}

void auditPresetShapeTests()
{
    resetMemoryFixture();
    VoiceLimitFixture f(48000); auto* division=f.engine.getDivisionByIndex(0);
    f.processor.suspendForEdit(true);
    division->addRankwave(aeolus::EngineGlobal::getInstance()->getStop(0));
    f.processor.suspendForEdit(false);
    division->postControl(aeolus::ControlAction::CapturePreset,0); f.block();
    check(!division->isPresetCaptured(0),"stale preset shape was captured");
    f.processor.suspendForEdit(true);
    division->ensurePresets(); f.engine.getSequencer()->initFromEngine();
    f.processor.suspendForEdit(false);
    division->postControl(aeolus::ControlAction::CapturePreset,0); f.block();
    check(division->isPresetCaptured(0),"prepared preset shape could not be captured");
    f.processor.suspendForEdit(true);
    auto saved=division->getPersistentState();
    division->addRankwave(aeolus::EngineGlobal::getInstance()->getStop(0));
    division->ensurePresets(); division->setPersistentState(saved);
    check(!division->isPresetCaptured(0),"shape-mismatched preset remained captured");
    division->enableStop(0,true); division->recallPreset(0);
    check(division->isStopEnabled(0),"invalidated preset silenced registration");
    f.processor.suspendForEdit(false);

}

}
