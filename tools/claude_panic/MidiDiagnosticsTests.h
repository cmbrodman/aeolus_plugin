// Copyright (C) 2026 Aeolus fork contributors
// SPDX-License-Identifier: GPL-3.0-or-later
void midiDiagnosticsTests()
{
    using namespace aeolus_midi_diag;
    const auto enOn=load(notes[Enqueue][0]),enOff=load(notes[Enqueue][1]);
    const auto deOn=load(notes[Dequeue][0]),deOff=load(notes[Dequeue][1]);
    juce::MidiMessageCollector collector;
    collector.reset(44100);
    auto on=juce::MidiMessage::noteOn(3,61,1.0f);
    auto off=juce::MidiMessage::noteOn(3,61,juce::uint8(0));
    on.setTimeStamp(juce::Time::getMillisecondCounterHiRes()*0.001);
    off.setTimeStamp(on.getTimeStamp());
    collector.addMessageToQueue(on);collector.addMessageToQueue(off);
    juce::MidiBuffer out;
    collector.removeNextBlockOfMessages(out,512);
    check(out.getNumEvents()==2,"diagnostics altered collector delivery");
    check(load(notes[Enqueue][0])==enOn+1 && load(notes[Enqueue][1])==enOff+1,
          "collector enqueue diagnostics missed zero-velocity note-off");
    check(load(notes[Dequeue][0])==deOn+1 && load(notes[Dequeue][1])==deOff+1,
          "collector dequeue diagnostics disagree");
    auto resetBefore=load(extra[ResetPrunedOff]);
    collector.addMessageToQueue(off);collector.reset(44100);
    check(load(extra[ResetPrunedOff])==resetBefore+1,"reset did not count discarded note-off");
    auto ageBefore=load(extra[AgePrunedOff]);
    off.setTimeStamp(juce::Time::getMillisecondCounterHiRes()*0.001);
    collector.addMessageToQueue(off);
    on.setTimeStamp(off.getTimeStamp()+2.0);
    collector.addMessageToQueue(on);
    check(load(extra[AgePrunedOff])==ageBefore+1,"age pruning did not count lost note-off");
    collector.reset(44100);
    auto windowBefore=load(extra[WindowPrunedOff]);
    off.setTimeStamp(juce::Time::getMillisecondCounterHiRes()*0.001);
    collector.addMessageToQueue(off);
    std::this_thread::sleep_for(std::chrono::milliseconds(4));
    out.clear();collector.removeNextBlockOfMessages(out,1);
    check(load(extra[WindowPrunedOff])==windowBefore+1 && out.isEmpty(),
          "time-window pruning did not count lost note-off");
    const unsigned char sysex[]{0xf0,1,2,3,4,5,6,0xf7};
    auto rawBefore=load(notes[Dequeue][0]);
    raw(Dequeue,sysex,sizeof(sysex));
    check(load(notes[Dequeue][0])==rawBefore,"sysex counted as a note");
    char text[1024]{};
    check(reply("DIAG",text,sizeof(text))>0 && std::string(text).find("DIAG 1 ")==0,
          "missing diagnostics snapshot");
    check(reply("NOTE 3 61",text,sizeof(text))>0,"missing per-key diagnostics");
    check(reply("NOTE 0 61",text,sizeof(text))==0 && reply("NOTE 1 128",text,sizeof(text))==0,
          "invalid per-key query accepted");
    // Real processor/engine note dispatch, including repeated releases, remains
    // covered by midiTests; counters must observe those actual paths as well.
    const auto pOff=load(notes[Processor][1]),eOff=load(notes[Engine][1]);
    const auto handled=load(extra[EngineOff]);
    midiTests();
    check(load(notes[Processor][1])>pOff && load(notes[Engine][1])>eOff && load(extra[EngineOff])>handled,
          "processor/engine release diagnostic hook missing");
}
