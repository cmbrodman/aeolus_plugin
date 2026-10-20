// Copyright (C) 2026 Aeolus fork contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "aeolus/callbackprofiler.h"

// Included inside AudioTests' anonymous namespace, using check()/pump().
void callbackProfilerTests()
{
    using Profiler = aeolus::CallbackProfiler;
    const auto directory = juce::File::getSpecialLocation(juce::File::tempDirectory)
        .getNonexistentChildFile("aeolus-profiler-test", {}, false);
    check(directory.createDirectory().wasOk(), "cannot create owned profiler test directory");
    struct Cleanup {
        juce::File directory;
        ~Cleanup() { directory.deleteRecursively(); }
    } cleanup {directory};
    const auto csv = directory.getChildFile("timing.csv");
    auto gate = std::make_shared<Profiler::WriterTestState>();
    auto profiler = Profiler::createForTesting(csv.getFullPathName(), gate);
    const auto waitUntil = [](const auto& ready) {
        const auto deadline = std::chrono::steady_clock::now()+std::chrono::seconds(3);
        while (!ready() && std::chrono::steady_clock::now()<deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        check(ready(), "profiler writer transition timed out");
    };
    waitUntil([&] { return gate->entered.load(); });
    heapOperations=0; countHeap=true;
    for (int i=0; i<6000; ++i) profiler->record(i, 1, 44100, 512, 12, 0);
    countHeap=false;
    check(heapOperations==0, "profiler callback allocated or freed");
    check(profiler->getDroppedCount()==1905, "blocked writer did not bound its queue and count overflow");
    struct Pulse : juce::Timer {
        int ticks=0;
        void timerCallback() override { ++ticks; }
    } pulse;
    pulse.startTimer(5);
    pump(80);
    pulse.stopTimer();
    check(pulse.ticks>=3, "blocked profiler writer prevented GUI timer progress");
    const auto before=std::chrono::steady_clock::now();
    profiler.reset();
    check(std::chrono::steady_clock::now()-before<std::chrono::milliseconds(500),
          "blocked writer made profiler shutdown unbounded");
    check(!gate->finished.load(), "blocked writer gate was not exercised");
    gate->paused.store(false);
    waitUntil([&] { return gate->finished.load(); });
    const auto text=csv.loadFileAsString();
    check(text.startsWith("sequence,start_ns,duration_ns,sample_rate,frames,voices,native_xruns"),
          "profiler CSV header changed");
    check(text.contains("0,0,1,44100.000,512,12,0") && text.contains("4094,4094,1,44100.000,512,12,0"),
          "profiler lost or reordered queued records during detached finalization");
    check(text.contains("# dropped_total=1905"), "profiler did not preserve overflow footer");
    check(!Profiler::create(csv.getFullPathName()), "profiler accepted an existing file");
    check(csv.loadFileAsString()==text, "profiler overwrote an existing file");
    // Simulate a path becoming invalid after construction but before background open.
    auto badGate=std::make_shared<Profiler::WriterTestState>();
    auto failed=Profiler::createForTesting(directory.getChildFile("absent/timing.csv").getFullPathName(),badGate);
    badGate->paused.store(false);
    waitUntil([&] { return failed->hasWriteError(); });
    failed.reset();
    waitUntil([&] { return badGate->finished.load(); });
    std::cout<<"Profiler: stalled writer leaves GUI/audio publication live; bounded teardown, overflow and I/O errors passed\n";
}
