// Copyright (C) 2026 Aeolus fork contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "PluginProcessor.h"
#include "aeolus/simd.h"
#include "aeolus/dsp/spatial.h"
#include <iostream>
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
void check(bool ok, const char* message)
{
    if (!ok) throw std::runtime_error(message);
}
void pump(int milliseconds)
{
    juce::MessageManager::getInstance()->runDispatchLoopUntil(milliseconds);
}
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
void spatialTests()
{
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
}

void* operator new(std::size_t size)
{
    if (countHeap) ++heapOperations;
    if (void* p = std::malloc(std::max(size, size_t(1)))) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* p) noexcept { if (countHeap && p) ++heapOperations; std::free(p); }
void operator delete[](void* p) noexcept { ::operator delete(p); }
void operator delete(void* p, std::size_t) noexcept { ::operator delete(p); }
void operator delete[](void* p, std::size_t) noexcept { ::operator delete(p); }

int main(int argc, char** argv)
{
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
        queueTests(); simdTests(); scalarConvolutionTest(); reverbTests(); concurrentReverbTests(); midiTests(); lifetimeTests();
        std::cout << "Audio/MIDI regressions passed\n";
    } catch (const std::exception& e) {
        countHeap = false;
        std::cerr << "FAIL: " << e.what() << '\n';
        result = 1;
    }
    juce::DeletedAtShutdown::deleteAll();
    return result;
}
