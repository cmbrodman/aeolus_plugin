// Copyright (C) 2026 Aeolus fork contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "ui/SequencerView.h"
#include "ui/StopEditorComponent.h"
#include "ui/MidiSettingsComponent.h"
#include "aeolus/simd.h"
#include "aeolus/simd_neon.h"
#include "aeolus/dsp/spatial.h"
#include "aeolus/dsp/delay.h"
#include "aeolus/dsp/convolve.h"
#include <algorithm>
#include <atomic>
#include <iomanip>
#include <limits>
#include <iostream>
#include <map>
#include <chrono>
#include <stdexcept>
#include <thread>
#include <cstdlib>
#include <new>

namespace aeolus::no_simd {
float mul_reduce(const float*, const float*, size_t);
void complex_mul_conj(float*, const float*, const float*, size_t);
void fft_step(float*, const float*, size_t);
}

namespace {
thread_local bool countHeap = false;
thread_local size_t heapOperations = 0;
// Counts heap operations on every thread (DSP workers included).
std::atomic<bool> countAllHeap { false };
std::atomic<size_t> allHeapOperations { 0 };
void check(bool ok, const char* message)
{
    if (!ok) throw std::runtime_error(message);
}
void pump(int milliseconds)
{
    juce::MessageManager::getInstance()->runDispatchLoopUntil(milliseconds);
}
#include "CallbackProfilerTests.h"

void queueTests()
{
    aeolus::RingBuffer<int, 7> q;
    for (int cycle = 0; cycle < 100; ++cycle) {
        for (int i = 0; i < 6; ++i) check(q.send(i), "queue capacity");
        check(!q.send(99) && q.count() == 6, "queue overflow/count after wrap");
        for (int i = 0, value; i < 6; ++i) {
            check(q.receive(value) && value == i, "queue FIFO");
        }
        check(q.count() == 0, "queue empty count");
    }
    aeolus::RingBuffer<int, 1024> concurrent;
    std::thread producer([&] {
        for (int i = 0; i < 100000; ++i)
            while (!concurrent.send(i)) std::this_thread::yield();
    });
    bool ordered = true;
    for (int i = 0, value; i < 100000; ++i) {
        while (!concurrent.receive(value)) std::this_thread::yield();
        ordered &= value == i;
    }
    producer.join();
    check(ordered, "concurrent SPSC order");
}
void simdTests()
{
#if AEOLUS_HAS_NEON
    check(aeolus::simd::mul_reduce_unaligned == &aeolus::neon::mul_reduce
          && aeolus::simd::complex_mul_conj == &aeolus::neon::complex_mul_conj
          && aeolus::simd::fft_step == &aeolus::neon::fft_step, "ARM64 NEON dispatch not selected");
    std::cout << "ARM64 NEON dispatch selected\n";
#endif
    alignas(32) float a[64], b[64], out[64];
    for (int i = 0; i < 64; ++i) { a[i] = std::sin(float(i)); b[i] = std::cos(float(i)); out[i] = a[i]; }
    aeolus::simd::mul_const_add(out, b, 0.37f, 64);
    for (int i = 0; i < 64; ++i)
        check(std::abs(out[i] - (a[i] + b[i] * 0.37f)) < 2e-6f, "SIMD mix mismatch");
    float expected = 0;
    for (int i = 1; i < 62; ++i) expected += a[i] * b[i];
    check(std::abs(aeolus::simd::mul_reduce_unaligned(a + 1, b + 1, 61) - expected) < 2e-5f,
          "unaligned SIMD remainder mismatch");
    aeolus::simd::complex_mul_conj(out, a, b, 64);
    for (int i = 0; i < 64; i += 2) {
        check(std::abs(out[i] - (a[i] * b[i] - a[i+1] * b[i+1])) < 2e-6f, "complex real mismatch");
        check(std::abs(out[i+1] - (-a[i] * b[i+1] - a[i+1] * b[i])) < 2e-6f, "complex imaginary mismatch");
    }
    std::cout << "CPU SSE3: " << aeolus::CPUTraits::get().sse3 << '\n';
}
std::vector<float> renderConvolutionReference()
{
    aeolus::dsp::Convolver convolution;
    juce::AudioBuffer<float> ir(2, 16384);
    for (int i = 0; i < ir.getNumSamples(); ++i) {
        ir.setSample(0, i, 0.004f * std::exp(-i / 3000.0f) * std::sin(i * 0.017f));
        ir.setSample(1, i, 0.004f * std::exp(-i / 3300.0f) * std::cos(i * 0.019f));
    }
    convolution.setLength(20480);
    convolution.prepareToPlay(44100, 64);
    convolution.setIR(ir);
    convolution.setDryWet(0.7f, 0.3f, true);
    convolution.setNonRealtime(true);
    std::vector<float> left(32768), right(32768);
    for (int i = 0; i < 32768; ++i) {
        left[i] = std::sin(i * 0.027f) * 0.1f + std::sin(i * 0.071f) * 0.05f;
        right[i] = std::cos(i * 0.023f) * 0.1f;
    }
    for (int i = 0; i < 32768; i += 257) {
        const int count = std::min(257, 32768 - i);
        convolution.process(left.data() + i, right.data() + i,
                            left.data() + i, right.data() + i, count);
    }
    left.insert(left.end(), right.begin(), right.end());
    return left;
}
void scalarConvolutionTest()
{
    // Run before any engine/background convolution exists. Each renderer joins
    // its idle worker before dispatch pointers change, so this is serialized.
    const auto dispatched = renderConvolutionReference();
    const auto dot = aeolus::simd::mul_reduce_unaligned;
    const auto multiply = aeolus::simd::complex_mul_conj;
    const auto fft = aeolus::simd::fft_step;
    aeolus::simd::mul_reduce_unaligned = &aeolus::no_simd::mul_reduce;
    aeolus::simd::complex_mul_conj = &aeolus::no_simd::complex_mul_conj;
    aeolus::simd::fft_step = &aeolus::no_simd::fft_step;
    const auto scalar = renderConvolutionReference();
    aeolus::simd::mul_reduce_unaligned = dot;
    aeolus::simd::complex_mul_conj = multiply;
    aeolus::simd::fft_step = fft;
    float peak = 0;
    for (size_t i = 0; i < scalar.size(); ++i) {
        const float difference = std::abs(scalar[i] - dispatched[i]);
        check(std::isfinite(difference), "nonfinite convolution reference difference");
        peak = std::max(peak, difference);
    }
    std::cout << "Scalar/dispatched convolution peak difference: " << peak << '\n';
    check(peak < 1e-5f, "SIMD convolution differs from scalar reference");
}
void profilerTests()
{
    const auto file = aeolus::getTestDataDirectory().getChildFile("callback-profile-test.csv");
    file.deleteFile();
    auto profiler = aeolus::CallbackProfiler::create(file.getFullPathName());
    check(profiler != nullptr, "profiler could not start for fresh test CSV");
    heapOperations = 0; countHeap = true;
    profiler->record(0,12000000,44100,512,399,0);
    countHeap = false;
    check(heapOperations == 0, "profiling allocated/freed on audio producer");
    profiler.reset();
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(3);
    juce::String text;
    do {
        text=file.loadFileAsString();
        if (text.contains("# dropped_total=")) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    } while (std::chrono::steady_clock::now()<deadline);
    check(text.contains("sequence,start_ns,duration_ns") && text.contains("# dropped_total=0"),
          "profiler did not finish its background CSV");
    check(text.contains("0,0,12000000,44100.000,512,399,0"), "profiler sample data changed");
    check(aeolus::CallbackProfiler::create(file.getFullPathName()) == nullptr, "profiler accepted an existing file");
    check(file.loadFileAsString()==text, "profiler overwrote an existing file");
}
void spatialTests()
{
#if AEOLUS_HAS_NEON && defined(AEOLUS_SPATIAL_NEON)
    std::cout << "Paired-ear NEON spatial candidate selected\n";
#endif
    float peak = 0;
    for (float rate : {44100.0f, 48000.0f}) {
        for (float position : {-8.0f, -0.15f, 0.0f, 0.15f, 8.0f}) {
            for (int alias = 0; alias < 4; ++alias) {
                aeolus::dsp::SpatialSource reference, block;
                for (auto* source : {&reference, &block}) {
                    source->setSampleRate(rate);
                    source->setSourcePosition(position, 5.0f);
                    source->recalculate(); source->reset();
                }
                // Include impulse, deterministic noise and a long silent tail;
                // alternate block and single-sample processing to check state.
                uint32_t random = 123;
                int sample = 0;
                for (int frames : {0, 1, 17, 64, 257, 1024, 63, 8192}) {
                    std::vector<float> input(frames), l(frames), r(frames), expectedL(frames), expectedR(frames);
                    for (int i = 0; i < frames; ++i, ++sample) {
                        random = random * 1664525u + 1013904223u;
                        input[i] = sample == 0 ? 1.0f : sample < 1400 ? float(int(random >> 8) - 8388608) / 8388608.0f : 0;
                        reference.tick(input[i], expectedL[i], expectedR[i]);
                    }
                    float* outL = alias == 1 ? input.data() : l.data();
                    float* outR = alias == 2 ? input.data() : alias == 3 ? outL : r.data();
                    heapOperations = 0; countHeap = true;
                    block.process(input.data(), outL, outR, frames);
                    countHeap = false;
                    check(heapOperations == 0, "spatial block allocated/freed");
                    for (int i = 0; i < frames; ++i) {
                        for (float error : {outL[i] - (alias == 3 ? expectedR[i] : expectedL[i]), outR[i] - expectedR[i]}) {
                            check(std::isfinite(error), "nonfinite spatial output");
                            peak = std::max(peak, std::abs(error));
                        }
                    }
                    float a, b, c, d;
                    reference.tick(0.1f, a, b); block.tick(0.1f, c, d);
                    peak = std::max({peak, std::abs(a-c), std::abs(b-d)});
                }
                reference.reset(); block.reset();
                for (int i = 0; i < 2048; ++i) {
                    float a, b, c, d, input = i == 0 ? 1.0f : 0.0f;
                    reference.tick(input, a, b); block.process(&input, &c, &d, 1);
                    peak = std::max({peak, std::abs(a-c), std::abs(b-d)});
                }
            }
        }
    }
    std::cout << "Spatial block/reference peak difference: " << peak << '\n';
    check(peak < 1e-6f, "spatial block changed output/history");
}
void spatialBenchmark()
{
    // Kernel microbenchmark, not a polyphony/xrun measurement. Warm both
    // variants, alternate timing order, consume output and report median/p99.
    juce::ScopedNoDenormals noDenormals;
    constexpr int voices = 300, frames = 64, rounds = 160;
    std::vector<aeolus::dsp::SpatialSource> reference(voices), block(voices);
    std::array<float, frames> input, left, right;
    for (int i = 0; i < frames; ++i) input[i] = std::sin(i * 0.17f) * 0.1f;
    for (int v = 0; v < voices; ++v)
        for (auto* source : {&reference[v], &block[v]}) {
            source->setSourcePosition(-8.0f + v * 16.0f / voices, 5.0f);
            source->recalculate(); source->reset();
        }
    volatile double checksum = 0;
    std::vector<double> times[2];
    for (int round = -20; round < rounds; ++round) {
        for (int order = 0; order < 2; ++order) {
            const int variant = (round + 20 + order) % 2;
            const auto start = std::chrono::steady_clock::now();
            auto& sources = variant == 0 ? reference : block;
            for (auto& source : sources) {
                if (variant == 0)
                    for (int i = 0; i < frames; ++i) source.tick(input[i], left[i], right[i]);
                else source.process(input.data(), left.data(), right.data(), frames);
                checksum += left[frames-1] + right[frames-1];
            }
            const auto us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now()-start).count();
            if (round >= 0) times[variant].push_back(us);
        }
    }
    for (int i = 0; i < 2; ++i) {
        std::sort(times[i].begin(), times[i].end());
        std::cout << "Spatial " << (i == 0 ? "tick reference" : "block")
                  << " 300 voices/64 frames: median_us=" << times[i][rounds/2]
                  << " p99_us=" << times[i][rounds*99/100] << " worst_us=" << times[i].back() << '\n';
    }
    std::cout << "Spatial median speedup: " << times[0][rounds/2]/times[1][rounds/2]
              << " checksum=" << checksum << '\n';
}
void reverbTests()
{
    for (float rate : {44100.0f, 48000.0f}) {
        aeolus::dsp::ReverbSwitch switched;
        juce::AudioBuffer<float> ir(2, 128);
        ir.clear(); ir.setSample(0, 0, 0.5f); ir.setSample(1, 0, 0.25f);
        switched.prepare(ir, true, rate);
        switched.setWet(0.4f);
        std::array<float, 256> left {}, right {};
        left[0] = right[0] = 1;
        heapOperations = 0; countHeap = true;
        switched.process(left.data(), right.data(), int(left.size()), true);
        countHeap = false;
        check(heapOperations == 0, "reverb adoption allocated/freed on callback");
        if (rate == 44100.0f) {
            // Preserve the inherited FIR head's two-sample wet latency.
            check(std::abs(left[0] - 1.0f) < 1e-5f && std::abs(right[0] - 1.0f) < 1e-5f,
                  "dry impulse changed");
            check(std::abs(left[2] - 0.2f) < 1e-5f && std::abs(right[2] - 0.1f) < 1e-5f,
                  "impulse convolution differs from delayed direct reference");
            for (int i = 1; i < 256; ++i)
                if (i != 2)
                    check(std::abs(left[i]) < 1e-5f && std::abs(right[i]) < 1e-5f, "unexpected impulse tail");
        }
        float wetSumLeft = -1.0f, wetSumRight = -1.0f;
        for (int i = 0; i < 256; ++i) {
            wetSumLeft += left[i];
            wetSumRight += right[i];
        }
        check(std::abs(wetSumLeft - 0.2f) < 0.005f && std::abs(wetSumRight - 0.1f) < 0.005f,
              "device-rate IR resampling changed convolution gain");
        check(std::abs(switched.tailSeconds() - 128.0f / 44100.0f) < 1.0f / rate, "IR duration changed with device rate");
        for (int change = 0; change < 12; ++change) {
            ir.setSample(0, 0, change * 0.02f);
            switched.prepare(ir, true, rate);
            for (int block = 0; block < 15; ++block) {
                left.fill(0.1f); right.fill(-0.1f);
                heapOperations = 0; countHeap = true;
                switched.process(left.data(), right.data(), 256, true);
                countHeap = false;
                check(heapOperations == 0, "reverb transition allocated/freed on callback");
                for (int i = 0; i < 256; ++i)
                    check(std::isfinite(left[i]) && std::abs(left[i]) < 1, "unstable reverb transition");
            }
            switched.collect();
        }
        // Also reclaim both a fading convolver and an unconsumed replacement.
        switched.prepare(ir, false, rate);
        switched.process(left.data(), right.data(), 1, true);
        switched.prepare(ir, true, rate);
    }
}
void concurrentReverbTests()
{
    aeolus::dsp::ReverbSwitch reverb;
    std::atomic<bool> finished {false};
    std::thread preparer([&] {
        juce::AudioBuffer<float> ir(2, 8192);
        ir.clear();
        ir.setSample(0, 0, 0.25f); ir.setSample(1, 0, 0.25f);
        ir.setSample(0, 6000, 0.1f); ir.setSample(1, 6000, 0.1f);
        for (int i = 0; i < 24; ++i) {
            reverb.prepare(ir, i % 2 == 0, 44100.0f);
            reverb.collect();
            std::this_thread::yield();
        }
        finished.store(true);
    });
    std::array<float, 256> left {}, right {};
    bool stable = true;
    int blocks = 0;
    while (!finished.load() || blocks < 64) {
        left.fill(0.01f); right.fill(-0.01f);
        heapOperations = 0; countHeap = true;
        reverb.process(left.data(), right.data(), 256, true);
        countHeap = false;
        stable &= heapOperations == 0;
        for (int i = 0; i < 256; ++i)
            stable &= std::isfinite(left[i]) && std::abs(left[i]) < 1;
        ++blocks;
        std::this_thread::yield();
    }
    preparer.join();
    check(stable, "concurrent IR preparation violated callback ownership/stability");
    reverb.collect();
}
void midiTests()
{
    for (float rate : {44100.0f, 48000.0f}) {
        AeolusAudioProcessor processor;
        processor.setNonRealtime(true);
        auto& engine = processor.getEngine();
        for (int d = 0; d < engine.getDivisionCount(); ++d) {
            auto* division = engine.getDivisionByIndex(d);
            for (int s = 0; s < division->getStopsCount(); ++s) division->enableStop(s, false);
        }
        auto* division = engine.getDivisionByIndex(0);
        check(division->getStopsCount() > 0, "fixture needs a stop");
        division->enableStop(0, true);
        division->setMIDIChannelsMask(1);
        processor.prepareToPlay(rate, 4096);
        processor.getParametersContainer().reverbWet->setValueNotifyingHost(0);
        juce::AudioBuffer<float> audio(aeolus::N_OUTPUT_CHANNELS, 4096);
        juce::MidiBuffer midi;
        midi.addEvent(juce::MidiMessage::noteOn(1, 60, 1.0f), 997);
        midi.addEvent(juce::MidiMessage::noteOff(1, 60), 3201);
        processor.processBlock(audio, midi);
        for (int ch = 0; ch < audio.getNumChannels(); ++ch)
            for (int i = 0; i < 997; ++i)
                check(std::abs(audio.getSample(ch, i)) <= 8.1e-5f, "MIDI sounded before its sample offset");
        check(audio.getMagnitude(997, 2204) > 1e-4f, "offset note did not sound");
        // A dropped GUI note-off must trigger panic rather than leave keys held.
        for (int i = 0; i < 1500; ++i) engine.postNoteEvent(true, 60, 1);
        engine.postNoteEvent(false, 60, 1);
        midi.clear();
        for (int block = 0; block < 220; ++block) processor.processBlock(audio, midi);
        check(engine.getVoiceCount() == 0, "note queue overflow left voices sounding");
        // A note shorter than one internal quantum must not leave a voice
        // permanently in Release with no playback pointer.
        midi.addEvent(juce::MidiMessage::noteOn(1, 61, 1.0f), 17);
        midi.addEvent(juce::MidiMessage::noteOff(1, 61), 18);
        processor.processBlock(audio, midi);
        midi.clear();
        for (int block = 0; block < 220; ++block) processor.processBlock(audio, midi);
        check(engine.getVoiceCount() == 0, "short note left an unreclaimable voice");
        engine.postNoteEvent(true, 60, 1);
        processor.panic();
        processor.processBlock(audio, midi);
        check(engine.getVoiceCount() == 0, "panic replayed an already queued GUI note-on");
        juce::AudioBuffer<float> empty(aeolus::N_OUTPUT_CHANNELS, 0);
        processor.processBlock(empty, midi);
        check(std::isfinite(processor.getProcessLoad()), "zero-length block corrupted meter");
    }
}
void lifetimeTests()
{
    {
        aeolus::Engine engine;
        engine.setWindowSize(901, 603);
        engine.requestSaveOrganState();
        // Timer delivery is asynchronous, and the first UI tick may also
        // prepare an IR. Observe the result with a deadline rather than assume
        // a loaded native CI runner will deliver six ticks within 400 ms.
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        bool saved = false;
        do {
            pump(20);
            const auto state = juce::JSON::parse(aeolus::getOrganStateFile());
            saved = int(state.getProperty("window_width", 0)) == 901;
        } while (!saved && std::chrono::steady_clock::now() < deadline);
        check(saved, "debounced state was not saved within five seconds");
    }
    for (int i = 0; i < 12; ++i) {
        aeolus::Engine engine;
        engine.requestSaveOrganState();
        if (i % 2) pump(60);
    }
    pump(400); // old raw-this callbacks would execute after destruction here
}
void delayBoundedResetTests()
{
    // resetForReadIndex() must be indistinguishable from reset() for every
    // access pattern the voices use, starting from arbitrary (even NaN) history.
    for (size_t size : {size_t(5), size_t(97), size_t(128), size_t(4096)}) {
        for (int pattern = 0; pattern < 2; ++pattern) {
            for (float delay : {0.0f, 0.5f, 1.0f, 3.0f, 63.0f, 64.0f, 64.75f, 95.0f, 96.0f, 127.0f, 1349.3f, 2697.9f, 4094.0f, 4095.0f}) {
                aeolus::dsp::DelayLine full(size), bounded(size);
                uint32_t random = 7u + uint32_t(size) + uint32_t(delay * 4);
                for (size_t i = 0; i < 3 * size + 11; ++i) {
                    random = random * 1664525u + 1013904223u;
                    const float dirty = (random & 15) == 0 ? std::numeric_limits<float>::quiet_NaN() : float(random >> 8);
                    full.write(dirty);
                    bounded.write(dirty);
                }
                full.reset();
                // Chiff reads floor(delay) and the next slot; voice/spatial delays read the index itself.
                const int bound = pattern == 0 ? int(delay) + 1 : int(delay);
                bounded.resetForReadIndex(bound);
                for (size_t i = 0; i < 3 * size + 5; ++i) {
                    random = random * 1664525u + 1013904223u;
                    const float input = float(int(random >> 8) - 8388608) / 8388608.0f;
                    float a, b;
                    if (pattern == 0) {
                        // Chiff: read before write, with feedback.
                        a = full.read(delay);
                        b = bounded.read(delay);
                        full.write(0.5f * a + input);
                        bounded.write(0.5f * b + input);
                    } else {
                        // Pipe delay / spatial: write then nearest read.
                        full.write(input);
                        bounded.write(input);
                        a = full.readNearest(int(delay));
                        b = bounded.readNearest(int(delay));
                    }
                    check(std::isfinite(a) && a == b, "bounded delay reset changed output");
                }
            }
        }
    }
}

//==============================================================================
// Batch renderer

void setEnvironment(const char* key, const char* value)
{
#ifdef _WIN32
    _putenv_s(key, value == nullptr ? "" : value); // An empty value removes the variable.
#else
    if (value == nullptr) unsetenv(key); else setenv(key, value, 1);
#endif
}

struct ScopedEnvironment
{
    explicit ScopedEnvironment(const std::map<std::string, std::string>& values)
    {
        for (const char* key : keys)
            setEnvironment(key, nullptr);
        for (const auto& [key, value] : values)
            setEnvironment(key.c_str(), value.c_str());
    }
    ~ScopedEnvironment()
    {
        for (const char* key : keys)
            setEnvironment(key, nullptr);
    }
    static constexpr const char* keys[] = {
        "AEOLUS_DSP_WORKERS", "AEOLUS_DSP_GROUP_SIZE", "AEOLUS_PER_VOICE_RANDOM",
        "AEOLUS_MAX_VOICES", "AEOLUS_DSP_WORKER_CPUS", "AEOLUS_DSP_BATCH_QUANTA", "AEOLUS_AUDIO_RT_PRIORITY", "AEOLUS_AUDIO_CPUS"
    };
};

struct RenderResult
{
    std::vector<float> left, right;
    juce::uint64 mispredictions = 0;
    int peakVoices = 0;
    int finalVoices = 0;
    int poolSize = 0;
    uint64_t rejectedNotes = 0;
    juce::uint64 triggers = 0;
    size_t heapOperations = 0;
    aeolus::DspTeam::Stats stats;
};

struct ScriptEvent
{
    double seconds;
    bool on;
    int channel;
    int note;
};

// Couplers from later divisions (II -> III, I -> II/III, Pedal -> manuals)
// exercise control changes that take effect one quantum into a batch.
RenderResult renderScript(const std::map<std::string, std::string>& environment, float rate, bool allStops, bool changeRegistration = true)
{
    ScopedEnvironment scoped(environment);
    RenderResult result;

    AeolusAudioProcessor processor;
    processor.setNonRealtime(true);
    auto& engine = processor.getEngine();
    result.poolSize = engine.getVoicePool().getMaxVoices();

    for (int d = 0; d < engine.getDivisionCount(); ++d) {
        auto* division = engine.getDivisionByIndex(d);
        division->setMIDIChannelsMask(1 << d);
        for (int s = 0; s < division->getStopsCount(); ++s)
            division->enableStop(s, allStops || (s % 3) == d % 3);
        for (int l = 0; l < division->getLinksCount(); ++l)
            division->enableLink(l, true);
        if (division->hasTremulant())
            division->setTremulantEnabled(true);
    }

    const std::vector<ScriptEvent> script = {
        {0.010, true, 2, 60}, {0.010, true, 2, 64}, {0.010, true, 2, 67},
        {0.020, true, 4, 36},
        {0.200, true, 3, 48}, {0.2001, true, 3, 55},
        {0.350, false, 2, 64}, {0.35002, true, 2, 64},
        {0.500, true, 1, 72}, {0.50001, false, 1, 72},
        {0.650, true, 1, 79},
        {0.900, false, 3, 48}, {0.900, false, 3, 55},
        {1.100, false, 2, 60}, {1.100, false, 2, 64}, {1.100, false, 2, 67},
        {1.150, false, 4, 36}, {1.300, false, 1, 79},
    };

    // Reprepare is idempotent for the published tuning generation.
    processor.prepareToPlay(rate, 1024);
    processor.getParametersContainer().reverbWet->setValueNotifyingHost(0);

    if (auto* team = engine.getDspTeam()) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!team->haveAllWorkersStarted() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        check(team->haveAllWorkersStarted(), "DSP workers did not start");
    }

    // JUCE's MidiKeyboardState grows its listener bookkeeping on first use.
    // Warm it up on channel 16, which no division listens to here.
    engine.getMidiKeyboardState().noteOn(16, 60, 1.0f);
    engine.getMidiKeyboardState().noteOff(16, 60, 0.0f);

    const int blockSizes[] = { 512, 333, 1024, 64, 7, 480, 1, 1000 };
    const int64_t totalSamples = int64_t(rate * 3.2);
    juce::AudioBuffer<float> audio(aeolus::N_OUTPUT_CHANNELS, 1024);
    juce::MidiBuffer midi;
    size_t nextEvent = 0;
    int block = 0;

    for (int64_t position = 0; position < totalSamples; ++block) {
        const int frames = (int) std::min<int64_t>(blockSizes[block % 8], totalSamples - position);
        midi.clear();
        while (nextEvent < script.size() && int64_t(script[nextEvent].seconds * rate) < position + frames) {
            const auto& e = script[nextEvent++];
            const int offset = (int) std::max<int64_t>(0, int64_t(e.seconds * rate) - position);
            midi.addEvent(e.on ? juce::MidiMessage::noteOn(e.channel, e.note, 1.0f)
                               : juce::MidiMessage::noteOff(e.channel, e.note), offset);
        }
        // Registration changes between blocks (disabled stops release,
        // re-enabled stops retrigger held keys).
        if (changeRegistration && block == 1500 * 44100 / int(rate) / 100) engine.getDivisionByIndex(1)->enableStop(0, false);
        if (changeRegistration && block == 1800 * 44100 / int(rate) / 100) engine.getDivisionByIndex(1)->enableStop(0, true);

        juce::AudioBuffer<float> view(audio.getArrayOfWritePointers(), audio.getNumChannels(), frames);
        allHeapOperations = 0;
        countAllHeap = true;
        processor.processBlock(view, midi);
        countAllHeap = false;
        result.heapOperations += allHeapOperations;
        for (int i = 0; i < frames; ++i) {
            result.left.push_back(view.getSample(0, i));
            result.right.push_back(view.getSample(1, i));
        }
        result.peakVoices = std::max(result.peakVoices, engine.getVoiceCount());
        position += frames;
    }

    result.rejectedNotes = engine.getRejectedNoteCount();
    result.triggers = engine.getVoicePool().getTriggerCount();
    result.mispredictions = engine.getBatchMispredictions();
    result.finalVoices = engine.getVoiceCount();
    if (auto* team = engine.getDspTeam())
        result.stats = team->getStats();
    return result;
}

void compareExact(const RenderResult& a, const RenderResult& b, const char* message)
{
    check(a.left.size() == b.left.size(), message);
    for (size_t i = 0; i < a.left.size(); ++i) {
        if (!(a.left[i] == b.left[i] && a.right[i] == b.right[i])) {
            std::cerr << "first difference at sample " << i << ": " << a.left[i] << " vs " << b.left[i]
                      << ", " << a.right[i] << " vs " << b.right[i] << '\n';
            check(false, message);
        }
    }
}

float peakDifference(const RenderResult& a, const RenderResult& b)
{
    float peak = 0;
    for (size_t i = 0; i < std::min(a.left.size(), b.left.size()); ++i)
        peak = std::max({peak, std::abs(a.left[i] - b.left[i]), std::abs(a.right[i] - b.right[i])});
    return peak;
}

float peakLevel(const RenderResult& r)
{
    float peak = 0;
    for (size_t i = 0; i < r.left.size(); ++i)
        peak = std::max({peak, std::abs(r.left[i]), std::abs(r.right[i])});
    return peak;
}

void modelReloadSafetyTests()
{
    ScopedEnvironment environment(std::map<std::string, std::string>{{"AEOLUS_DSP_WORKERS","3"}});
    AeolusAudioProcessor processor;
    processor.prepareToPlay(44100,512);
    auto& engine=processor.getEngine();
    auto* division=engine.getDivisionByIndex(0);
    division->setMIDIChannelsMask(1);division->enableStop(0,true);
    auto stops=aeolus::getTestDataDirectory().getChildFile("Stops").findChildFiles(juce::File::findFiles,false,"*.json");
    check(!stops.isEmpty(),"reload fixture has no test-owned model");
    std::atomic<bool> running {true}, release {false};
    std::thread audio([&] {
        juce::AudioBuffer<float> buffer(aeolus::N_OUTPUT_CHANNELS,512);
        juce::MidiBuffer midi;midi.addEvent(juce::MidiMessage::noteOn(1,60,1.0f),17);
        while(running.load()) {
            {
                const juce::ScopedLock lock(processor.getCallbackLock());
                if(!processor.isSuspended()) {
                    if(release.exchange(false)) midi.addEvent(juce::MidiMessage::noteOff(1,60),17);
                    processor.processBlock(buffer,midi);midi.clear();
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });
    struct Join { std::atomic<bool>& running;std::thread& thread;~Join(){running=false;thread.join();} } join {running,audio};
    auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
    while(engine.getAudioCompleted()==0 && std::chrono::steady_clock::now()<deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    check(engine.getAudioCompleted()>0,"reload audio fixture did not start");
    check(engine.getVoiceCount()>0,"reload fixture did not create live voices");
    auto* global=aeolus::EngineGlobal::getInstance();
    global->reloadStopFile(stops[0]);
    check(global->hasDeferredStopReloads(),"replacement was not deferred while pipes were sounding");
    check(engine.getVoiceCount()>0 && !engine.isEmergencyMuted(),"model reload cut an active note or latched Panic");
    const auto clone=stops[0].getSiblingFile("test-new-"+juce::Uuid().toString()+".json");
    check(stops[0].copyFileTo(clone),"could not create test-owned model variant");
    global->reloadStopFile(clone);
    check(global->getStopByName(clone.getFileNameWithoutExtension())!=nullptr,"fresh variant was not immediately available");
    check(engine.getVoiceCount()>0,"fresh model creation interrupted active voices");
    release=true;
    deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
    while(global->hasDeferredStopReloads() && std::chrono::steady_clock::now()<deadline) pump(20);
    processor.suspendForEdit(true);
    check(!global->hasDeferredStopReloads() && engine.getVoiceCount()==0,"deferred model did not reload after its tails finished");
    check(!engine.isEmergencyMuted(),"idle model reload latched Panic");
    processor.suspendForEdit(false);
    clone.deleteFile();
}

void safetyFooterTests()
{
    // Simulated sysfs only: tests must never change a real system governor.
    juce::TemporaryFile temporary;
    const auto root = temporary.getFile();
    struct FixtureCleanup { juce::File path; ~FixtureCleanup() { path.deleteRecursively(); } } cleanup {root};
    const auto active = root.getChildFile("policy0");
    const auto inactive = root.getChildFile("policy4");
    check(active.createDirectory().wasOk() && inactive.createDirectory().wasOk(), "cannot prepare CPU policy fixture");
    active.getChildFile("affected_cpus").replaceWithText("0 1 2 3");
    active.getChildFile("scaling_governor").replaceWithText("performance\n");
    inactive.getChildFile("affected_cpus").replaceWithText("\n");
    auto paths = ui::PerformanceModeButton::policies(root);
    check(paths.size()==1 && ui::PerformanceModeButton::allPerformance(paths),
          "inactive CPU policy prevents performance detection");
    active.getChildFile("scaling_governor").replaceWithText("conservative\n");
    check(!ui::PerformanceModeButton::allPerformance(paths), "wrong governor shown as performance");
    active.getChildFile("scaling_governor").deleteFile();
    check(!ui::PerformanceModeButton::allPerformance(paths), "unreadable active policy shown as performance");
    auto* global=aeolus::EngineGlobal::getInstance();
    const bool original=global->isKeyboardVisible();
    for(const bool visible:{true,false}) {
        global->setKeyboardVisible(visible);
        AeolusAudioProcessor processor;
        std::unique_ptr<juce::AudioProcessorEditor> editor(processor.createEditor());
        editor->setSize(640,480);
        editor->setVisible(true);
        juce::TextButton* panic=nullptr;juce::TextButton* resume=nullptr;
        juce::TextButton* performance=nullptr;
        juce::MidiKeyboardComponent* keyboard=nullptr;ui::SequencerView* sequencer=nullptr;
        for(auto* child:editor->getChildren()) {
            if(auto* button=dynamic_cast<juce::TextButton*>(child)) {
                if(button->getName()=="temporaryPerformanceMode") performance=button;
                if(button->getButtonText()=="PANIC") panic=button;
                if(button->getButtonText()=="Resume audio") resume=button;
            }
            if(auto* item=dynamic_cast<juce::MidiKeyboardComponent*>(child)) keyboard=item;
            if(auto* item=dynamic_cast<ui::SequencerView*>(child)) sequencer=item;
        }
        check(panic && resume && keyboard && sequencer,"global safety controls missing");
        check(performance && !performance->getBounds().intersects(resume->getBounds()),
              "temporary performance control missing or overlaps Resume");
        check(!panic->getBounds().intersects(resume->getBounds()),"Panic and Resume overlap");
        check(sequencer->getBottom()<=panic->getY(),"sequencer overlaps safety footer");
        if(visible) check(keyboard->getBottom()<=panic->getY(),"keyboard overlaps Panic");
        check(!resume->isEnabled(),"Resume enabled before Panic acknowledgement");
        auto* console=dynamic_cast<AeolusAudioProcessorEditor*>(editor.get());
        check(console!=nullptr,"standalone console editor missing");
        console->onSequencerEnterProgramMode();
        check(editor->getComponentAt(panic->getBounds().getCentre())==panic,
              "preset programming overlay blocks Panic");
        panic->triggerClick();pump(20);
        check(processor.getEngine().isEmergencyPanicPending(),"console Panic did not request emergency silence");
        console->onSequencerLeaveProgramMode();
        // Bare Xvfb has no window manager and JUCE popup atoms are unavailable.
        // Enable this workflow on an authenticated desktop for native UI checks.
        if(!visible && std::getenv("AEOLUS_TEST_POPUP_WORKFLOW")) {
            for(auto* child:editor->getChildren())
                if(auto* button=dynamic_cast<juce::DrawableButton*>(child);button && button->getName()=="settingsButton") button->triggerClick();
            pump(20);
            editor.reset();
            juce::PopupMenu::dismissAllActiveMenus();pump(20);
        }
    }
    global->setKeyboardVisible(original);
}

void realtimeConfigTests()
{
    check(aeolus::RealtimeConfig::parseCpuList("1-3,6")==std::vector<int>({1,2,3,6}),"CPU range parsing failed");
    for(const auto* invalid:{"0,","1-","x","2048"}) {
        bool rejected=false;
        try { aeolus::RealtimeConfig::parseCpuList(invalid); } catch(const std::invalid_argument&) { rejected=true; }
        check(rejected,"malformed CPU list silently removed affinity");
    }
    ScopedEnvironment environment(std::map<std::string, std::string>{{"AEOLUS_DSP_WORKERS","99999999999999999999999999"}});
#if JUCE_LINUX && !AEOLUS_MULTIBUS_OUTPUT
    bool rejected=false;
    try { aeolus::RealtimeConfig::fromEnvironment(); } catch(const std::invalid_argument&) { rejected=true; }
    check(rejected,"overflowed integer setting accepted");
#endif
}

void batchRendererTests()
{
#if JUCE_LINUX && !AEOLUS_MULTIBUS_OUTPUT
    for (float rate : {44100.0f, 48000.0f}) {
        // Serial renderer with per-voice noise is the reference. The batch
        // renderer with one voice per group sums in the same order and must
        // match it exactly.
        const auto serial = renderScript({{"AEOLUS_PER_VOICE_RANDOM", "1"}}, rate, false);
        // Fixture check: per-voice noise makes the serial renderer repeatable.
        const auto again = renderScript({{"AEOLUS_PER_VOICE_RANDOM", "1"}}, rate, false);
        compareExact(serial, again, "serial renderer with per-voice noise is not repeatable");
        const auto k1 = renderScript({{"AEOLUS_DSP_WORKERS", "0"}, {"AEOLUS_DSP_GROUP_SIZE", "1"}, {"AEOLUS_DSP_BATCH_QUANTA", "1"}}, rate, false);
        compareExact(serial, k1, "batch renderer (1 quantum, group 1) differs from serial");
        const auto batchG1 = renderScript({{"AEOLUS_DSP_WORKERS", "0"}, {"AEOLUS_DSP_GROUP_SIZE", "1"}}, rate, false);
        const auto batchG1Workers = renderScript({{"AEOLUS_DSP_WORKERS", "3"}, {"AEOLUS_DSP_GROUP_SIZE", "1"}}, rate, false);

        check(peakLevel(serial) > 1e-3f, "batch fixture is silent");
        check(serial.peakVoices > 20 && serial.peakVoices < serial.poolSize, "batch fixture voice count out of range");
        check(serial.finalVoices == 0, "serial fixture left voices sounding");
        compareExact(serial, batchG1, "batch renderer (group 1, no workers) differs from serial");
        compareExact(serial, batchG1Workers, "batch renderer (group 1, 3 workers) differs from serial");

        // Grouped summation is deterministic regardless of thread count.
        const auto g8w0 = renderScript({{"AEOLUS_DSP_WORKERS", "0"}}, rate, false);
        const auto g8w1 = renderScript({{"AEOLUS_DSP_WORKERS", "1"}}, rate, false);
        const auto g8w3 = renderScript({{"AEOLUS_DSP_WORKERS", "3"}}, rate, false);
        compareExact(g8w0, g8w1, "batch renderer output depends on worker count (1)");
        compareExact(g8w0, g8w3, "batch renderer output depends on worker count (3)");
        const auto refused=renderScript({{"AEOLUS_DSP_WORKERS","3"},{"AEOLUS_DSP_WORKER_CPUS","1023"}},rate,false);
        compareExact(g8w0,refused,"worker-policy fallback changed the rendered output");
        check(refused.stats.jobsByWorkers==0,"dispatched jobs despite refused worker affinity");

        const float rounding = peakDifference(serial, g8w0);
        std::cout << "Batch renderer at " << rate << " Hz: peak voices " << serial.peakVoices
                  << ", grouped-summation peak difference " << rounding
                  << " (signal peak " << peakLevel(serial) << "), worker jobs "
                  << g8w3.stats.jobsByWorkers << "/" << g8w3.stats.jobs << '\n';
        check(rounding < 1e-5f, "grouped summation differs by more than rounding");

        for (const auto* r : {&serial, &batchG1, &batchG1Workers, &g8w0, &g8w1, &g8w3}) {
            check(r->mispredictions == 0, "batch renderer rendered quanta ahead of MIDI events");
            check(r->finalVoices == 0, "batch renderer left voices sounding");
            check(r->heapOperations == 0, "processing allocated or freed heap memory");
        }

        // Bounded ring clears must match the original full clears, including
        // for recycled voices whose rings hold old audio.
        {
            const std::map<std::string, std::string> reuse { {"AEOLUS_PER_VOICE_RANDOM", "1"}, {"AEOLUS_MAX_VOICES", "128"} };
            const auto bounded = renderScript(reuse, rate, false);
            aeolus::dsp::setFullVoiceClearForTesting(true);
            const auto full = renderScript(reuse, rate, false);
            aeolus::dsp::setFullVoiceClearForTesting(false);
            check(bounded.peakVoices < bounded.poolSize && bounded.triggers > juce::uint64(bounded.poolSize),
                  "voice reuse fixture did not recycle voices");
            compareExact(full, bounded, "bounded voice ring clears changed output");
        }

        // An undersized pool rejects whole notes, never partial mixtures.
        const auto exhausted = renderScript({{"AEOLUS_DSP_WORKERS", "3"}, {"AEOLUS_MAX_VOICES", "64"}}, rate, true);
        check(exhausted.peakVoices <= 64 && exhausted.rejectedNotes > 0,
              "undersized pool did not report whole-note rejection");
        check(exhausted.finalVoices == 0 && exhausted.mispredictions == 0 && exhausted.heapOperations == 0,
              "batch renderer misbehaved at pool exhaustion");
    }

#endif
}

//==============================================================================

void serialEmergencyPanicTests()
{
    ScopedEnvironment environment({});
    for(const float rate:{44100.0f,48000.0f}) {
        AeolusAudioProcessor processor;
        auto& engine=processor.getEngine();
        engine.getDivisionByIndex(0)->setMIDIChannelsMask(1);
        engine.getDivisionByIndex(0)->enableStop(0,true);
        processor.prepareToPlay(rate,512);
        juce::AudioBuffer<float> audio(aeolus::N_OUTPUT_CHANNELS,512);
        juce::MidiBuffer midi;
        midi.addEvent(juce::MidiMessage::noteOn(1,60,1.0f),17);
        processor.processBlock(audio,midi);midi.clear();
        check(engine.getVoiceCount()>0,"serial Panic fixture had no voices");
        processor.panic();processor.processBlock(audio,midi);
        check(engine.isEmergencyMuted() && !engine.isEmergencyPanicPending(),"serial panic did not latch and acknowledge");
        check(engine.getVoiceCount()==0 && audio.getMagnitude(0,512)==0,"serial panic left audible voices");
        check(engine.authorizeEmergencyResume(engine.getPanicAcknowledged()),"serial resume authorization failed");
        processor.releaseResources();processor.prepareToPlay(rate,512);
        check(!engine.isEmergencyMuted(),"serial Resume failed");
    }
}

void emergencyPanicTests()
{
#if JUCE_LINUX && !AEOLUS_MULTIBUS_OUTPUT
    for (const int workers : {0, 3}) for (const float rate : {44100.0f, 48000.0f}) {
        ScopedEnvironment environment({{"AEOLUS_DSP_WORKERS", std::to_string(workers)}, {"AEOLUS_MAX_VOICES", "1024"}});
        AeolusAudioProcessor processor;
        auto& engine = processor.getEngine();
        for (int d=0; d<engine.getDivisionCount(); ++d) {
            auto* division = engine.getDivisionByIndex(d);
            division->setMIDIChannelsMask(1);
            for (int s=0; s<division->getStopsCount(); ++s) division->enableStop(s, true);
            for (int l=0; l<division->getLinksCount(); ++l) division->enableLink(l, false);
        }
        processor.prepareToPlay(rate,512);
        processor.getParametersContainer().reverbWet->setValueNotifyingHost(.42f);
        juce::AudioBuffer<float> audio(aeolus::N_OUTPUT_CHANNELS,512);
        juce::MidiBuffer midi;
        for (int note : {45,47,48,50,52,53,79,81,83,84}) midi.addEvent(juce::MidiMessage::noteOn(1,note,1.0f),0);
        for (int block=0;block<32;++block) {processor.processBlock(audio,midi);midi.clear();}
        check(engine.getVoiceCount()>400,"panic setup did not create dense registration");
        engine.setPanicTestDelay(2000);
        std::atomic<bool> observed {false};
        std::thread requester([&] {
            const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
            while (!engine.getPanicTestStarts() && std::chrono::steady_clock::now()<deadline) std::this_thread::yield();
            observed.store(engine.getPanicTestStarts()>0);
            processor.panic();
        });
        heapOperations=0;countHeap=true;
        processor.processBlock(audio,midi);
        countHeap=false;
        requester.join();
        check(heapOperations==0,"emergency panic allocated on the audio coordinator");
        engine.setPanicTestDelay(0);
        // 48 kHz can consume an existing interpolator sample before a job begins.
        if (engine.isEmergencyPanicPending()) processor.processBlock(audio,midi);
        check(observed.load(),"panic request did not overlap an in-flight job");
        check(!engine.isEmergencyPanicPending(),"panic was not acknowledged after worker completion");
        check(engine.getAudioStarted()>0 && engine.getAudioCompleted()==engine.getAudioStarted(),"callback heartbeat did not finish during panic");
        check(engine.isAudioActive(),"prepared processor did not publish active device state");
        check(engine.isEmergencyMuted() && engine.getVoiceCount()==0,"panic did not latch silence and reclaim voices");
        for (int block=0;block<12;++block) {
            engine.postNoteEvent(true, 73, 1);
            midi.addEvent(juce::MidiMessage::noteOn(1,72,1.0f),17);
            processor.processBlock(audio,midi);midi.clear();
            check(audio.getMagnitude(0,audio.getNumSamples())==0.0f,"panic leaked dry/reverb audio");
            check(engine.getVoiceCount()==0,"queued MIDI retriggered while panic was latched");
        }
        processor.releaseResources();
        processor.prepareToPlay(rate,512);
        check(engine.isEmergencyMuted(),"device changes bypassed latched panic without explicit Resume");
        check(engine.authorizeEmergencyResume(engine.getPanicAcknowledged()),"explicit resume authorization failed");
        engine.requestEmergencyPanic();
        processor.releaseResources();
        processor.prepareToPlay(rate,512);
        check(engine.isEmergencyMuted(),"stale Resume cleared a newer panic");
        processor.processBlock(audio,midi);
        check(!engine.isEmergencyPanicPending(),"newer panic failed acknowledgement");
        check(engine.authorizeEmergencyResume(engine.getPanicAcknowledged()),"latest Resume authorization failed");
        processor.releaseResources();
        processor.prepareToPlay(rate,512);
        check(!engine.isEmergencyMuted(),"device reprepare failed to resume after acknowledged panic");
        processor.processBlock(audio,midi);
        check(engine.getVoiceCount()==0,"notes queued while muted replayed after resume");
        check(audio.getMagnitude(0,audio.getNumSamples())<=8.1e-5f,"old audio leaked after resume");
        midi.addEvent(juce::MidiMessage::noteOn(1,60,1.0f),17);
        bool sounded=false;
        for (int block=0;block<32;++block) {
            processor.processBlock(audio,midi);midi.clear();
            sounded |= audio.getMagnitude(0,audio.getNumSamples())>1e-5f;
        }
        check(sounded && engine.getVoiceCount()>0,"new notes did not play after explicit resume");
    }
    std::cout<<"Emergency panic: delayed in-flight jobs acknowledged, voices reclaimed, wet output silent, resume passed\n";

#endif
}

// Offline engine benchmark: aeolus_tests --benchmark-engine [notes] [seconds]
// Uses the environment as given (AEOLUS_DSP_WORKERS etc.).

struct WindowStats
{
    std::vector<double> ms;
    void print(const char* name, double budget) const
    {
        if (ms.empty()) { std::cout << name << ": no blocks\n"; return; }
        auto sorted = ms;
        std::sort(sorted.begin(), sorted.end());
        const auto at = [&](double p) { return sorted[std::min(sorted.size() - 1, size_t(p * double(sorted.size() - 1) + 0.5))]; };
        const auto over = std::count_if(ms.begin(), ms.end(), [&](double v) { return v > budget; });
        std::cout << std::fixed << std::setprecision(3) << name << ": blocks " << ms.size()
                  << ", p50 " << at(0.5) << " ms, p99 " << at(0.99) << " ms, max " << sorted.back()
                  << " ms, over budget " << over << '\n';
    }
};

void engineBenchmark(int numNotes, double holdSeconds)
{
    constexpr float rate = 44100.0f;
    constexpr int blockSize = 512;
    const double budget = 1000.0 * blockSize / rate;

    // AEOLUS_BENCHMARK_FULL_VOICE_CLEAR=1 restores the original full ring
    // clears for an A/B comparison within one test build.
    const bool fullClear = std::getenv("AEOLUS_BENCHMARK_FULL_VOICE_CLEAR") != nullptr;
    aeolus::dsp::setFullVoiceClearForTesting(fullClear);

    AeolusAudioProcessor processor;
    auto& engine = processor.getEngine();
    for (int d = 0; d < engine.getDivisionCount(); ++d) {
        auto* division = engine.getDivisionByIndex(d);
        division->setMIDIChannelsMask(1);
        for (int s = 0; s < division->getStopsCount(); ++s) division->enableStop(s, true);
        for (int l = 0; l < division->getLinksCount(); ++l) division->enableLink(l, false);
    }
    processor.prepareToPlay(rate, blockSize);
    processor.getParametersContainer().reverbWet->setValueNotifyingHost(0);

    std::vector<int> notes;
    for (int i = 0; i < numNotes; ++i) {
        const int note = 36 + (numNotes > 1 ? (i * 48) / (numNotes - 1) : 0);
        if (notes.empty() || notes.back() != note) notes.push_back(note);
    }

    const auto& config = engine.getRealtimeConfig();
    std::cout << "Engine benchmark: " << notes.size() << " notes, all stops, 44100 Hz / " << blockSize
              << " frames (budget " << budget << " ms), voice pool " << engine.getVoicePool().getMaxVoices() << ", "
              << (config.isBatchRendering() ? "batch renderer, " + std::to_string(config.dspWorkers) + " workers, group "
                                                  + std::to_string(config.dspGroupSize)
                                            : std::string("serial renderer"))
              << (fullClear || aeolus::dsp::useFullVoiceClear() ? ", full voice ring clears" : ", bounded voice ring clears") << '\n';

    juce::AudioBuffer<float> audio(aeolus::N_OUTPUT_CHANNELS, blockSize);
    juce::MidiBuffer midi;
    WindowStats attack, sustain, release;
    int peakVoices = 0;
    const int attackBlocks = int(rate / blockSize);
    const int holdBlocks = int(holdSeconds * rate / blockSize);

    using Clock = std::chrono::steady_clock;
    const auto timeBlock = [&] {
        const auto start = Clock::now();
        processor.processBlock(audio, midi);
        midi.clear();
        return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
    };

    for (int i = 0; i < 40; ++i) timeBlock(); // warm up
    for (int note : notes) midi.addEvent(juce::MidiMessage::noteOn(1, note, 1.0f), 0);
    for (int b = 0; b < holdBlocks; ++b) {
        const double ms = timeBlock();
        (b < attackBlocks ? attack : sustain).ms.push_back(ms);
        peakVoices = std::max(peakVoices, engine.getVoiceCount());
    }
    for (int note : notes) midi.addEvent(juce::MidiMessage::noteOff(1, note), 0);
    for (int b = 0; b < int(10 * rate / blockSize) && (b == 0 || engine.getVoiceCount() > 0); ++b)
        release.ms.push_back(timeBlock());

    auto& report = aeolus::rt::audioThreadReport();
    aeolus::rt::ThreadPolicyStatus status;
    status.priorityError = report.priorityError.load();
    status.affinityError = report.affinityError.load();
    std::cout << "Calling thread: " << aeolus::rt::describe(status, config.audioPriority, config.audioCpus) << '\n';
    if (auto* team = engine.getDspTeam())
        if (team->getNumWorkers() > 0)
            std::cout << "Workers: " << team->describeWorkers() << '\n';
    std::cout << "Peak voices: " << peakVoices << '\n';
    attack.print("Attack (first second)", budget);
    sustain.print("Sustain", budget);
    release.print("Release (until silent)", budget);
    if (auto* team = engine.getDspTeam()) {
        const auto stats = team->getStats();
        std::cout << "Team: " << stats.batches << " batches, " << stats.jobsByWorkers << "/" << stats.jobs
                  << " jobs on workers, waited in " << stats.waits << " batches, max wait "
                  << double(stats.waitNanosMax) / 1000.0 << " us\n";
    }
}
}

void* operator new(std::size_t size)
{
    if (countAllHeap.load(std::memory_order_relaxed)) ++allHeapOperations;
    if (countHeap) ++heapOperations;
    if (void* p = std::malloc(std::max(size, size_t(1)))) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* p) noexcept
{
    if (p && countAllHeap.load(std::memory_order_relaxed)) ++allHeapOperations;
    if (countHeap && p) ++heapOperations;
    std::free(p);
}
void operator delete[](void* p) noexcept { ::operator delete(p); }
void operator delete(void* p, std::size_t) noexcept { ::operator delete(p); }
void operator delete[](void* p, std::size_t) noexcept { ::operator delete(p); }

#include "PerformanceSafetyTests.h"
#include "RenderReference.h"
#include "MemoryLifetimeTests.h"
#include "ConvolutionOwnershipTests.h"
#include "AuditFollowupTests.h"

int main(int argc, char** argv)
{
#if JUCE_LINUX
    if (argc==3 && std::string(argv[1])=="--short-write-probe")
        return shortWriteProbe(juce::File(argv[2]));
#endif
    juce::ScopedJuceInitialiser_GUI gui;
    int result = 0;
    try {
        spatialTests();
        if (argc == 2 && std::string(argv[1]) == "--benchmark-spatial") {
            spatialBenchmark();
            return 0;
        }
        const auto fixture = aeolus::getTestDataDirectory();
        fixture.createDirectory();
        fixture.getChildFile("organ_config.json").replaceWithData(BinaryData::default_organ_json, BinaryData::default_organ_jsonSize);
        fixture.getChildFile("organ_state.json").replaceWithText("{}");
        if (argc >= 2 && std::string(argv[1]) == "--benchmark-engine") {
            engineBenchmark(argc >= 3 ? std::atoi(argv[2]) : 10, argc >= 4 ? std::atof(argv[3]) : 8.0);
            juce::DeletedAtShutdown::deleteAll();
            return 0;
        }
        aeolus::EngineGlobal::getInstance()->setVoiceLimitSetting(0);
        if (argc == 3 && std::string(argv[1]) == "--export-safety-reference") {
            exportSafetyReference(argv[2]); juce::DeletedAtShutdown::deleteAll(); return 0;
        }
        performanceMeterTests(); voiceAdmissionTests(); performanceSettingsTests();
        delayBoundedResetTests();
        callbackProfilerTests();
        safetyFooterTests(); modelReloadSafetyTests(); realtimeConfigTests(); batchRendererTests(); serialEmergencyPanicTests(); emergencyPanicTests();
        profilerTests(); queueTests(); simdTests(); scalarConvolutionTest(); reverbTests(); concurrentReverbTests(); midiTests(); lifetimeTests();
        memoryLifetimeTests();
        convolutionOwnershipTests(); convolutionSetupTests();
        auditKeyTests(); auditDataTests(); auditControlTests(); auditConcurrentControls(); auditRejectedLayoutTests(); auditPresetShapeTests();
        std::cout << "Audio/MIDI regressions passed\n";
    } catch (const std::exception& e) {
        countHeap = false;
        std::cerr << "FAIL: " << e.what() << '\n';
        result = 1;
    }
    juce::DeletedAtShutdown::deleteAll();
    return result;
}
