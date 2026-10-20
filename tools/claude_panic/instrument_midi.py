#!/usr/bin/env python3
"""Instrument a fresh copy of the validated HEALTH 2 source. Never edit JUCE itself."""
from pathlib import Path
import argparse,re,shutil
p=argparse.ArgumentParser();p.add_argument('source',type=Path);p.add_argument('juce',type=Path);a=p.parse_args()
s=a.source.resolve();j=a.juce.resolve();here=Path(__file__).resolve().parent
shutil.copy2(here/'midi_diagnostics.h',s/'Source/aeolus/midi_diagnostics.h')
header=s/'Source/aeolus/midi_diagnostics.h'
def edit(path,old,new):
 t=path.read_text();assert t.count(old)==1,(str(path),old,t.count(old));path.write_text(t.replace(old,new))
def include(path):
 t=path.read_text();path.write_text('#include "aeolus/midi_diagnostics.h"\n'+t)
for file in ['PluginProcessor.cpp','aeolus/engine.cpp','aeolus/division.cpp','aeolus/voice.cpp','aeolus/paniccontrol.cpp']:include(s/'Source'/file)
edit(s/'Source/PluginProcessor.cpp','void AeolusAudioProcessor::processMidiMessage(const MidiMessage& msg)\n{','void AeolusAudioProcessor::processMidiMessage(const MidiMessage& msg)\n{\n    aeolus_midi_diag::event(aeolus_midi_diag::Processor,msg);')
edit(s/'Source/aeolus/engine.cpp','void Engine::processMIDIMessage(const MidiMessage& message)\n{','void Engine::processMIDIMessage(const MidiMessage& message)\n{\n    aeolus_midi_diag::event(aeolus_midi_diag::Engine,message);')
edit(s/'Source/aeolus/engine.cpp','void Engine::noteOff(int note, int midiChannel)\n{','void Engine::noteOff(int note, int midiChannel)\n{\n    aeolus_midi_diag::bump(aeolus_midi_diag::EngineOff);')
edit(s/'Source/aeolus/engine.cpp','_noteQueueOverflow.store(true);','{ aeolus_midi_diag::bump(aeolus_midi_diag::QueueOverflow); _noteQueueOverflow.store(true); }')
edit(s/'Source/aeolus/division.cpp','    releaseNoteInternal(note);','    aeolus_midi_diag::bump(aeolus_midi_diag::DivisionOff);\n    releaseNoteInternal(note);')
edit(s/'Source/aeolus/division.cpp','        if (voice->isForNote(note))\n            voice->release();','        if (voice->isForNote(note)) {\n            aeolus_midi_diag::bump(aeolus_midi_diag::ReleaseMatched);\n            voice->release();\n        }')
edit(s/'Source/aeolus/voice.cpp','void Voice::release()\n{','void Voice::release()\n{\n    aeolus_midi_diag::bump(aeolus_midi_diag::VoiceRelease);')
edit(s/'Source/aeolus/voice.cpp','        voice->trigger(state);','        aeolus_midi_diag::bump(aeolus_midi_diag::PoolTrigger);\n        voice->trigger(state);')
edit(s/'Source/aeolus/voice.cpp','    // No more voices.','    aeolus_midi_diag::bump(aeolus_midi_diag::PoolFailure);\n    // No more voices.')
edit(s/'Source/aeolus/voice.cpp','    _idleVoices.append(voice);','    aeolus_midi_diag::bump(aeolus_midi_diag::PoolReturn);\n    _idleVoices.append(voice);')
edit(s/'Source/aeolus/paniccontrol.cpp','                if (std::strcmp(command, "HEALTH") == 0) {','''                char diagnostic[1024] {};
                const int diagnosticSize=aeolus_midi_diag::reply(command,diagnostic,sizeof(diagnostic));
                if (diagnosticSize>0) {
                    ::sendto(fd,diagnostic,size_t(diagnosticSize),MSG_NOSIGNAL,
                             reinterpret_cast<sockaddr*>(&client),length);
                    continue;
                }
                if (std::strcmp(command, "HEALTH") == 0) {''')
# Generate private amalgamation wrappers using original include roots. JUCE notices stay intact.
w=s/'diagnostic-juce';w.mkdir()
def wrapper(module,target,patch):
 root=j/'modules'/module;orig=root/target
 text=orig.read_text();text=patch(text)
 targetcopy=w/Path(target).name;targetcopy.write_text('#include "'+str(header)+'"\n'+text)
 main=root/(module+'.cpp');text=main.read_text()
 def replace(m):
  name=m.group(1);path=targetcopy if name==target else root/name
  return '#include "'+str(path)+'"'
 text=re.sub(r'#include\s+"([^"]+)"',replace,text)
 (w/(module+'.cpp')).write_text(text)
def collector(t):
 def sub(old,new):
  nonlocal t
  assert t.count(old)==1,old;t=t.replace(old,new)
 sub('    sampleRate = newSampleRate;','    aeolus_midi_diag::bump(aeolus_midi_diag::CollectorReset);\n    sampleRate = newSampleRate;')
 sub('    sampleRate = newSampleRate;\n    incomingMessages.clear();','    sampleRate = newSampleRate;\n    for(const auto meta: incomingMessages) if(aeolus_midi_diag::offRaw(meta.data,meta.numBytes)) aeolus_midi_diag::bump(aeolus_midi_diag::ResetPrunedOff);\n    incomingMessages.clear();')
 sub('    incomingMessages.addEvent (message, sampleNumber);','    aeolus_midi_diag::event(aeolus_midi_diag::Enqueue,message);\n    incomingMessages.addEvent (message, sampleNumber);')
 sub('        incomingMessages.clear (0, sampleNumber - (int) sampleRate);','''    {
        for (const auto meta : incomingMessages)
            if (meta.samplePosition>=0 && meta.samplePosition<sampleNumber-(int)sampleRate && aeolus_midi_diag::noteRaw(meta.data,meta.numBytes))
                aeolus_midi_diag::bump(aeolus_midi_diag::AgePruned);
        for (const auto meta : incomingMessages)
            if (meta.samplePosition>=0 && meta.samplePosition<sampleNumber-(int)sampleRate && aeolus_midi_diag::offRaw(meta.data,meta.numBytes))
                aeolus_midi_diag::bump(aeolus_midi_diag::AgePrunedOff);
        incomingMessages.clear (0, sampleNumber - (int) sampleRate);
    }''')
 sub('                iter = incomingMessages.findNextSamplePosition (startSample);','''                iter = incomingMessages.findNextSamplePosition (startSample);
                for(auto old=incomingMessages.cbegin();old!=iter;++old)
                    if(aeolus_midi_diag::noteRaw((*old).data,(*old).numBytes)) aeolus_midi_diag::bump(aeolus_midi_diag::WindowPruned);
                for(auto old=incomingMessages.cbegin();old!=iter;++old)
                    if(aeolus_midi_diag::offRaw((*old).data,(*old).numBytes)) aeolus_midi_diag::bump(aeolus_midi_diag::WindowPrunedOff);''')
 sub('                const auto pos =','                aeolus_midi_diag::raw(aeolus_midi_diag::Dequeue,meta.data,meta.numBytes);\n                const auto pos =')
 sub('            for (const auto metadata : incomingMessages)\n                destBuffer.addEvent','            for (const auto metadata : incomingMessages) {\n                aeolus_midi_diag::raw(aeolus_midi_diag::Dequeue,metadata.data,metadata.numBytes);\n                destBuffer.addEvent')
 sub('metadata.samplePosition + startSample));','metadata.samplePosition + startSample));\n            }')
 return t
wrapper('juce_audio_devices','midi_io/juce_MidiMessageCollector.cpp',collector)
def player(t):
 old='    messageCollector.addMessageToQueue (message);'
 assert t.count(old)==1
 return t.replace(old,'    aeolus_midi_diag::event(aeolus_midi_diag::Device,message);\n'+old)
wrapper('juce_audio_utils','players/juce_AudioProcessorPlayer.cpp',player)
print(s)
shutil.copy2(here/'MidiDiagnosticsTests.h',s/'tests/MidiDiagnosticsTests.h')
test=s/'tests/AudioTests.cpp'
t=test.read_text();t='#include "aeolus/midi_diagnostics.h"\n'+t
pos=t.index('int main(');t=t[:pos]+'#include "MidiDiagnosticsTests.h"\n'+t[pos:]
old=' concurrentReverbTests(); midiTests(); lifetimeTests();'
assert t.count(old)==1
test.write_text(t.replace(old,' concurrentReverbTests(); midiDiagnosticsTests(); lifetimeTests();'))
