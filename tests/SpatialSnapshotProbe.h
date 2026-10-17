// Copyright (C) 2026 Aeolus fork contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "MulticoreProbe.h"
#include <semaphore.h>
#include <cerrno>

namespace multicore_probe {
// Offline prototype of speculative spatial rendering. Workers see snapshots,
// never canonical state. The caller never waits: late results are discarded.
// Fixed geometry here guarantees vector copies reuse preallocated capacity.
// A live implementation would also need explicit capacity/geometry handling.
class SnapshotTeam {
public:
    SnapshotTeam(std::vector<Job>& canonical, int participantCount, int firstCpu, int injectedDelayUs = 0)
        : canonical(canonical), scratch(canonical), participants(participantCount), delayUs(injectedDelayUs) {
        try {
            for (int lane = 1; lane < participants; ++lane) {
                auto& w = workers[lane-1];
                if (sem_init(&w.wake, 0, 0) != 0) throw std::runtime_error("sem_init failed");
                w.initialized = true;
                w.thread = std::thread([this, lane, firstCpu] {
                    bool announced = false;
                    try {
                        Affinity affinity(firstCpu < 0 ? -1 : firstCpu + lane);
                        juce::ScopedNoDenormals noDenormals;
                        auto& w = workers[lane-1];
                        ready.fetch_add(1, std::memory_order_release); announced = true;
                        for (;;) {
                            int error;
                            do { error = sem_wait(&w.wake); } while (error != 0 && errno == EINTR);
                            if (error != 0) throw std::runtime_error("sem_wait failed");
                            if (stop.load(std::memory_order_acquire)) return;
                            const auto epoch = w.requested.load(std::memory_order_acquire);
                            if (delayUs && lane == 1 && epoch % 7 == 1)
                                std::this_thread::sleep_for(std::chrono::microseconds(delayUs));
                            for (int i = int(scratch.size())*lane/participants;
                                 i < int(scratch.size())*(lane+1)/participants; ++i) {
                                int expected = queued;
                                if (state[i].compare_exchange_strong(expected, running, std::memory_order_acq_rel))
                                    scratch[i].render();
                                state[i].store(done, std::memory_order_release);
                            }
                            w.finished.store(epoch, std::memory_order_release);
                        }
                    } catch (...) {
                        failed.store(true, std::memory_order_release);
                        if (!announced) ready.fetch_add(1, std::memory_order_release);
                    }
                });
            }
            while (ready.load(std::memory_order_acquire) != participants-1) std::this_thread::yield();
            if (failed.load()) throw std::runtime_error("snapshot worker startup failed");
        } catch (...) { shutdown(); throw; }
    }
    ~SnapshotTeam() { shutdown(); }
    void run() {
        if (failed.load(std::memory_order_acquire)) throw std::runtime_error("snapshot worker failed");
        const int size = int(canonical.size());
        for (int lane = 1; lane < participants; ++lane) {
            auto& w = workers[lane-1];
            const auto previous = w.requested.load(std::memory_order_relaxed);
            active[lane-1] = w.finished.load(std::memory_order_acquire) == previous;
            if (!active[lane-1]) { ++busyLanes; continue; }
            for (int i = size*lane/participants; i < size*(lane+1)/participants; ++i) {
                scratch[i].source = canonical[i].source;
                scratch[i].input = canonical[i].input;
                state[i].store(queued, std::memory_order_relaxed);
            }
            w.requested.store(previous+1, std::memory_order_release);
            if (sem_post(&w.wake) != 0) throw std::runtime_error("sem_post failed");
        }
        int lane = 1;
        for (int i = 0; i < size; ++i) {
            if (participants == 1 || i < size/participants) {
                canonical[i].render();
                continue;
            }
            while (lane < participants-1 && i >= size*(lane+1)/participants) ++lane;
            if (active[lane-1] && state[i].load(std::memory_order_acquire) == done) {
                // Worker has finished this slot. Swap is constant-time; both
                // sources already own equally sized/capacious delay buffers.
                std::swap(canonical[i].source, scratch[i].source);
                canonical[i].output = scratch[i].output;
                ++adopted;
            } else {
                if (active[lane-1]) {
                    int expected = queued;
                    state[i].compare_exchange_strong(expected, cancelled, std::memory_order_acq_rel);
                }
                canonical[i].render(); // Original state/input; no waiting or stale output.
                ++fallbacks;
            }
        }
    }
    void clearStats() { adopted = fallbacks = busyLanes = 0; }
    size_t adopted = 0, fallbacks = 0, busyLanes = 0;
private:
    void shutdown() {
        stop.store(true, std::memory_order_release);
        for (auto& w : workers) if (w.initialized) sem_post(&w.wake);
        for (auto& w : workers) {
            if (w.thread.joinable()) w.thread.join();
            if (w.initialized) { sem_destroy(&w.wake); w.initialized = false; }
        }
    }
    enum { queued, running, done, cancelled };
    struct Worker {
        sem_t wake;
        bool initialized = false;
        std::thread thread;
        alignas(64) std::atomic<unsigned> requested {0}, finished {0};
    };
    std::vector<Job>& canonical;
    std::vector<Job> scratch;
    const int participants, delayUs;
    std::array<Worker, 3> workers;
    std::array<std::atomic<int>, 512> state {};
    std::array<bool, 3> active {};
    std::atomic<int> ready {0};
    std::atomic<bool> failed {false}, stop {false};
};

inline void verifySnapshot(int voices, int count, int firstCpu) {
    auto jobs = makeJobs(voices), reference = jobs;
    SnapshotTeam team(jobs, count, firstCpu, 2000);
    for (int round = 0; round < 96; ++round) {
        if (round == 8)
            for (int v = 0; v < voices; ++v) {
                jobs[v].input.fill(0); reference[v].input.fill(0);
            }
        team.run();
        for (int v = 0; v < voices; ++v) {
            reference[v].render();
            if (std::memcmp(jobs[v].output.data(), reference[v].output.data(), sizeof(jobs[v].output)) != 0)
                throw std::runtime_error("snapshot/fallback output differs from serial");
        }
    }
    std::cout << "snapshot_check voices=" << voices << " participants=" << count
              << " delayed_worker_us=2000 adopted=" << team.adopted
              << " fallbacks=" << team.fallbacks << " busy_lanes=" << team.busyLanes
              << " exact_output=yes\n";
}

inline void runSnapshot(int firstCpu) {
    Affinity affinity(firstCpu);
    juce::ScopedNoDenormals noDenormals;
    volatile double checksum = 0;
    std::cout << "OFFLINE snapshot prototype; no caller completion wait; fixed geometry only.\n";
    for (int trial = 0; trial < 3; ++trial)
        for (int order = 0; order < 4; ++order) {
            const int count = trial % 2 ? 4-order : order+1;
            verifySnapshot(33, count, firstCpu); verifySnapshot(450, count, firstCpu);
            auto jobs = makeJobs(450);
            SnapshotTeam team(jobs, count, firstCpu);
            constexpr int samples = 400, warmup = 40;
            std::vector<double> times;
            times.reserve(samples);
            std::array<float, 128> mixed {};
            for (int sample = -warmup; sample < samples; ++sample) {
                if (sample == 0) team.clearStats();
                const auto start = std::chrono::steady_clock::now();
                for (int quantum = 0; quantum < 8; ++quantum) {
                    team.run();
                    mixed.fill(0);
                    for (auto& job : jobs)
                        for (int i = 0; i < 128; ++i) mixed[i] += job.output[i];
                    double sum = 0;
                    for (float value : mixed) sum += value;
                    checksum += sum;
                }
                const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now()-start).count();
                if (sample >= 0) times.push_back(us);
            }
            std::sort(times.begin(), times.end());
            std::cout << "trial=" << trial << " participants=" << count << " voices=450 frames=512 p50_us="
                      << times[samples/2] << " p99_us=" << times[samples*99/100] << " worst_us=" << times.back()
                      << " adopted=" << team.adopted << " fallbacks=" << team.fallbacks
                      << " busy_lanes=" << team.busyLanes << " exact_output=yes\n" << std::flush;
        }
    std::cout << "checksum=" << checksum << '\n';
}
} // namespace multicore_probe
