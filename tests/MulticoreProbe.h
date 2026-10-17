// Copyright (C) 2026 Aeolus fork contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// OFFLINE experiment only. The rendezvous below can wait for a preempted
// worker without a deadline. It must not be used by an audio callback.
#include "aeolus/dsp/spatial.h"
#include <atomic>
#include <array>
#include <chrono>
#include <cstring>
#include <iostream>
#include <thread>
#include <vector>
#if defined(__linux__)
#include <pthread.h>
#include <sched.h>
#endif

namespace multicore_probe {
struct alignas(64) Job {
    aeolus::dsp::SpatialSource source;
    std::array<float, 64> input {};
    std::array<float, 128> output {};
    void render() { source.process(input.data(), output.data(), output.data() + 64, 64); }
};

class Affinity {
public:
    explicit Affinity(int cpu) {
#if defined(__linux__)
        valid = pthread_getaffinity_np(pthread_self(), sizeof(previous), &previous) == 0;
        cpu_set_t mask;
        CPU_ZERO(&mask); CPU_SET(cpu, &mask);
        if (!valid || pthread_setaffinity_np(pthread_self(), sizeof(mask), &mask) != 0)
            throw std::runtime_error("cannot pin probe thread to requested CPU");
#else
        juce::ignoreUnused(cpu);
        throw std::runtime_error("the affinity probe currently requires Linux");
#endif
    }
    ~Affinity() {
#if defined(__linux__)
        if (valid) pthread_setaffinity_np(pthread_self(), sizeof(previous), &previous);
#endif
    }
private:
#if defined(__linux__)
    cpu_set_t previous {};
    bool valid = false;
#endif
};

// Fixed persistent participants; callback analogue participates as lane zero.
// Static slices avoid an atomic per chunk; dynamic slices let other lanes
// steal work that has not started. Neither scheme rescues an in-flight job
// when its worker is descheduled. Busy polling is intentional for this upper-
// bound experiment, not a proposed idle policy for the application.
class Team {
public:
    Team(std::vector<Job>& jobs, int count, int chunk, int firstCpu)
        : jobs(jobs), count(count), chunk(chunk) {
        try {
            for (int lane = 1; lane < count; ++lane)
                workers.emplace_back([this, lane, firstCpu] {
                    try {
                        Affinity affinity(firstCpu + lane);
                        juce::ScopedNoDenormals noDenormals;
                        ready.fetch_add(1, std::memory_order_release);
                        unsigned seen = 0;
                        while (!stop.load(std::memory_order_relaxed)) {
                            const auto current = epoch.load(std::memory_order_acquire);
                            if (current == seen) { relax(); continue; }
                            seen = current;
                            render(lane);
                            done[lane].value.store(current, std::memory_order_release);
                        }
                    } catch (...) {
                        failed.store(true, std::memory_order_relaxed);
                        ready.fetch_add(1, std::memory_order_release);
                    }
                });
            while (ready.load(std::memory_order_acquire) != count - 1) std::this_thread::yield();
            if (failed.load()) throw std::runtime_error("probe worker affinity failed");
        } catch (...) { shutdown(); throw; }
    }
    ~Team() { shutdown(); }
    void run() {
        if (count == 1) { render(0); return; }
        next.store(0, std::memory_order_relaxed);
        const auto current = epoch.load(std::memory_order_relaxed) + 1;
        epoch.store(current, std::memory_order_release);
        render(0);
        for (int lane = 1; lane < count; ++lane)
            while (done[lane].value.load(std::memory_order_acquire) != current) relax();
    }
private:
    static void relax() {
#if defined(__aarch64__)
        __asm__ __volatile__("yield");
#elif defined(__i386__) || defined(__x86_64__)
        __builtin_ia32_pause();
#else
        std::this_thread::yield();
#endif
    }
    void render(int lane) {
        const int size = int(jobs.size());
        if (chunk == 0 || count == 1) {
            for (int i = size * lane / count; i < size * (lane + 1) / count; ++i) jobs[i].render();
        } else {
            for (;;) {
                const int begin = next.fetch_add(chunk, std::memory_order_relaxed);
                if (begin >= size) break;
                for (int i = begin; i < std::min(size, begin + chunk); ++i) jobs[i].render();
            }
        }
    }
    void shutdown() {
        stop.store(true, std::memory_order_relaxed);
        for (auto& worker : workers) if (worker.joinable()) worker.join();
    }
    struct alignas(64) Completion { std::atomic<unsigned> value {0}; };
    std::vector<Job>& jobs;
    const int count, chunk;
    std::vector<std::thread> workers;
    std::array<Completion, 4> done;
    alignas(64) std::atomic<unsigned> epoch {0};
    alignas(64) std::atomic<int> next {0};
    std::atomic<int> ready {0};
    std::atomic<bool> stop {false}, failed {false};
};

inline std::vector<Job> makeJobs(int voices) {
    std::vector<Job> jobs(voices);
    for (int v = 0; v < voices; ++v) {
        jobs[v].source.setSourcePosition(-8.0f + v * 16.0f / voices, 5.0f);
        jobs[v].source.recalculate(); jobs[v].source.reset();
        for (int i = 0; i < 64; ++i)
            jobs[v].input[i] = 0.1f * std::sin(i * 0.17f + v * 0.13f);
    }
    return jobs;
}

inline void verify(int voices, int count, int chunk, int firstCpu) {
    auto jobs = makeJobs(voices), reference = jobs;
    Team team(jobs, count, chunk, firstCpu);
    // Includes excitation, short final chunk, and long silent spatial tails.
    for (int round = 0; round < 96; ++round) {
        if (round == 8)
            for (int v = 0; v < voices; ++v) {
                jobs[v].input.fill(0); reference[v].input.fill(0);
            }
        team.run();
        for (int v = 0; v < voices; ++v) {
            reference[v].render();
            if (std::memcmp(jobs[v].output.data(), reference[v].output.data(), sizeof(jobs[v].output)) != 0)
                throw std::runtime_error("multicore spatial output differs from serial");
        }
    }
}

inline void run(int firstCpu) {
    Affinity affinity(firstCpu);
    juce::ScopedNoDenormals noDenormals;
    std::cout << "OFFLINE spatial-only probe; busy workers; no realtime/scheduling guarantee.\n"
              << "CPU range=" << firstCpu << ".." << firstCpu + 3 << "\n";
    volatile double checksum = 0;
    for (int trial = 0; trial < 3; ++trial) {
        // Reverse order on alternate trials to expose heating/order bias.
        for (int order = 0; order < 4; ++order) {
            const int count = trial % 2 ? 4 - order : order + 1;
            for (const int chunk : {0, 16}) {
                if (count == 1 && chunk != 0) continue;
                verify(33, count, chunk, firstCpu);
                verify(450, count, chunk, firstCpu);
                auto jobs = makeJobs(450);
                Team team(jobs, count, chunk, firstCpu);
                constexpr int samples = 400, warmup = 40;
                std::vector<double> times;
                times.reserve(samples);
                std::array<float, 128> mixed {};
                for (int sample = -warmup; sample < samples; ++sample) {
                    const auto start = std::chrono::steady_clock::now();
                    for (int quantum = 0; quantum < 8; ++quantum) {
                        team.run();
                        // Preserve original voice summation order on lane zero.
                        mixed.fill(0);
                        for (auto& job : jobs)
                            for (int i = 0; i < 128; ++i) mixed[i] += job.output[i];
                        checksum += mixed[0] + mixed[127];
                    }
                    const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count();
                    if (sample >= 0) times.push_back(us);
                }
                std::sort(times.begin(), times.end());
                std::cout << "trial=" << trial << " participants=" << count << " chunk=" << chunk
                          << " voices=450 frames=512 p50_us=" << times[samples/2]
                          << " p99_us=" << times[samples*99/100] << " worst_us=" << times.back()
                          << " exact_output=yes\n" << std::flush;
            }
        }
    }
    std::cout << "checksum=" << checksum << '\n';
}
} // namespace multicore_probe
