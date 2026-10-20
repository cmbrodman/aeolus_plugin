// Copyright (C) 2026 Aeolus fork contributors
// SPDX-License-Identifier: GPL-3.0-or-later
namespace {
struct ConvolutionGate : aeolus::Worker::Job {
    std::atomic<bool> entered{false}, release{false};
    void run() override {
        entered.store(true);
        while (!release.load()) std::this_thread::yield();
    }
};
struct EmptyConvolutionJob : aeolus::Worker::Job { void run() override {} };
void convolutionOwnershipTests()
{
    using Conv = aeolus::dsp::EquallyPartitionedConvolver<64>;
    for (int mode = 0; mode < 3; ++mode) {
        Conv reference(4), actual(4);
        ConvolutionGate gate;
        EmptyConvolutionJob empty;
        aeolus::Worker worker;
        // Joining the worker precedes destruction of every queued job.
        struct Cleanup {
            ConvolutionGate& gate; aeolus::Worker& worker;
            ~Cleanup() { gate.release.store(true); worker.stop(); }
        } cleanup{gate, worker};
        worker.start();
        if (mode != 0) {
            check(worker.addJob(&gate), "convolution gate enqueue failed");
            const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            while (!gate.entered.load() && std::chrono::steady_clock::now() < until)
                std::this_thread::yield();
            check(gate.entered.load(), "convolution gate failed to start");
        }
        if (mode == 2) {
            int accepted = 0;
            while (worker.addJob(&empty)) ++accepted;
            check(accepted > 0, "convolution queue saturation fixture failed");
        }
        actual.setWorker(&worker);
        for (int i = 0; i < 256; ++i) {
            const float ir = float((i * 37) % 61 - 30) / 300.0f;
            reference.feedIr(ir); actual.feedIr(ir);
        }
        float difference = 0, energy = 0;
        size_t heap = 0;
        for (int i = 0; i < 16384; ++i) {
            const float x = i < 8192 ? float((i * 13) % 41 - 20) / 40.0f : 0;
            heapOperations = 0; countHeap = true;
            const float expected = reference.tick(x), rendered = actual.tick(x);
            countHeap = false; heap += heapOperations;
            difference = std::max(difference, std::abs(expected - rendered));
            energy += std::abs(rendered);
            // Allow an old job to finish after many input-buffer rotations.
            if (i == 4096) gate.release.store(true);
            if (i % 64 == 0) std::this_thread::yield();
            if (mode == 0 && actual.testPendingJobs()) {
                const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(2);
                while (actual.testPendingJobs() && std::chrono::steady_clock::now() < until)
                    std::this_thread::yield();
                check(!actual.testPendingJobs(), "normal convolution worker stalled");
            }
        }
        check(difference < 1e-6f, "late/rejected convolution job corrupted output or tail");
        check(energy > 1, "empty convolution ownership render");
        if (mode == 0) check(actual.testWorkerResults() > 0, "worker results were never consumed");
        check(heap == 0, "convolution fallback allocated on audio thread");
        gate.release.store(true);
        worker.stop();
        const auto completed=actual.testWorkerResults();
        worker.start();
        for (int i=0;i<1024;++i) {
            const float expected=reference.tick(0.1f), rendered=actual.tick(0.1f);
            check(std::abs(expected-rendered)<1e-6f,"worker restart lost convolution output");
            const auto until=std::chrono::steady_clock::now()+std::chrono::seconds(2);
            while(actual.testPendingJobs() && std::chrono::steady_clock::now()<until) std::this_thread::yield();
        }
        check(actual.testWorkerResults()>completed,"purged convolution job permanently disabled offload");
        worker.stop();
        worker.start(); // stopped workers must be reusable after setup
        ConvolutionGate restarted;
        restarted.release.store(true);
        check(worker.addJob(&restarted), "restarted worker rejected job");
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (!restarted.entered.load() && std::chrono::steady_clock::now() < until)
            std::this_thread::yield();
        worker.stop();
        check(restarted.entered.load(), "worker failed to restart after setup");
    }
}

void convolutionSetupTests()
{
    for (bool resize : {false, true}) {
        ConvolutionGate gate;
        aeolus::dsp::Convolver actual, reference;
        juce::AudioBuffer<float> ir(2, 16384);
        ir.clear(); ir.setSample(0, 6000, 0.3f); ir.setSample(1, 9000, 0.2f);
        for (auto* conv : {&actual, &reference}) {
            conv->setLength(16384); conv->prepareToPlay(44100, 64);
            conv->setIR(ir); conv->setDryWet(0, 1, true);
        }
        reference.setNonRealtime(true);
        struct ReleaseGate { ConvolutionGate& gate; ~ReleaseGate() { gate.release.store(true); } } releaseGate{gate};
        check(actual.testWorker().addJob(&gate), "setup gate enqueue failed");
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (!gate.entered.load() && std::chrono::steady_clock::now() < until)
            std::this_thread::yield();
        check(gate.entered.load(), "setup worker gate failed to start");
        std::array<float, 64> l{}, r{};
        l.fill(0.1f); r.fill(-0.1f);
        for (int i = 0; i < 256; ++i)
            actual.process(l.data(), r.data(), l.data(), r.data(), l.size());
        check(actual.testWorker().hasPendingJobs(), "setup fixture has no queued convolution jobs");
        ir.clear(); ir.setSample(0, 4500, 0.2f); ir.setSample(1, 5000, -0.1f);
        std::atomic<bool> setupStarted{false}, setupDone{false};
        std::thread setup([&] {
            setupStarted.store(true);
            if (resize) { actual.setLength(8192); actual.prepareToPlay(44100, 64); }
            actual.setIR(ir);
            setupDone.store(true);
        });
        while (!setupStarted.load()) std::this_thread::yield();
        // stop() clears running before joining. The gate cannot return yet,
        // so observing this transition proves setup actually reached the join.
        const auto joinDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (actual.testWorker().isRunning() && !setupDone.load()
               && std::chrono::steady_clock::now() < joinDeadline)
            std::this_thread::yield();
        const bool reachedJoin = !actual.testWorker().isRunning() && !setupDone.load();
        gate.release.store(true);
        setup.join();
        check(reachedJoin, "setup did not join the gated worker before reuse");
        if (resize) { reference.setLength(8192); reference.prepareToPlay(44100, 64); }
        reference.setIR(ir); reference.setNonRealtime(true);
        float difference = 0;
        for (int i = 0; i < 512; ++i) {
            l.fill(i < 128 ? 0.1f : 0); r.fill(i < 128 ? -0.1f : 0);
            auto expectedL = l, expectedR = r;
            actual.process(l.data(), r.data(), l.data(), r.data(), l.size());
            reference.process(expectedL.data(), expectedR.data(), expectedL.data(), expectedR.data(), l.size());
            for (size_t j = 0; j < l.size(); ++j)
                difference = std::max({difference, std::abs(l[j]-expectedL[j]), std::abs(r[j]-expectedR[j])});
        }
        check(difference < 1e-6f, "pending jobs survived IR replacement or reprepare");
    }
}
}
