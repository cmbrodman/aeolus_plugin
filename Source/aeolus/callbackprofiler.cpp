// Copyright (C) 2026 Aeolus fork contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "callbackprofiler.h"

namespace aeolus {
std::unique_ptr<CallbackProfiler> CallbackProfiler::create(const juce::String& path)
{
    if (path.isEmpty()) return {};
    if (!juce::File::isAbsolutePath(path)) return {};
    const juce::File file(path);
    // Never truncate an existing file or mistake an organ/settings file for
    // a telemetry destination. Select a fresh .csv path for each launch.
    if (file.exists() || file.getFileExtension() != ".csv") return {};
    auto output = file.createOutputStream();
    if (output == nullptr || !output->openedOk()) return {};
    return std::unique_ptr<CallbackProfiler>(new CallbackProfiler(std::move(output)));
}
CallbackProfiler::CallbackProfiler(std::unique_ptr<juce::FileOutputStream> output)
    : _output(std::move(output))
{
    _output->writeText("# block_delay=" + juce::String(AEOLUS_BLOCK_DELAY) + "\n", false, false, nullptr);
    _output->writeText("sequence,start_ns,duration_ns,sample_rate,frames,voices,native_xruns\n",
                       false, false, nullptr);
    startTimer(200);
}
CallbackProfiler::~CallbackProfiler()
{
    const juce::MessageManagerLock messageLock;
    stopTimer();
    timerCallback(); // producer must already be stopped by the processor owner
    _output->writeText("# dropped_total=" + juce::String(_dropped.load()) + "\n", false, false, nullptr);
    _output->flush();
}
void CallbackProfiler::record(int64_t start, int64_t duration, double rate,
                              int frames, int voices, int xruns) noexcept
{
    if (!_queue.send({_sequence++, start, duration, rate, frames, voices, xruns}))
        _dropped.fetch_add(1, std::memory_order_relaxed);
}
void CallbackProfiler::timerCallback()
{
    juce::String text;
    text.preallocateBytes(32768);
    const auto available = _queue.count();
    Sample sample {};
    for (size_t i = 0; i < available && _queue.receive(sample); ++i)
        text << juce::String(sample.sequence) << ',' << juce::String(sample.startNs) << ','
             << juce::String(sample.durationNs) << ',' << juce::String(sample.sampleRate, 3) << ','
             << sample.frames << ',' << sample.voices << ',' << sample.nativeXruns << '\n';
    if (text.isNotEmpty()) {
        _output->writeText(text, false, false, nullptr);
        _output->flush();
    }
}
}
