// Copyright (C) 2026 Aeolus fork contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "spatialbatch.h"
#include <atomic>
#include <array>
#include <thread>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#if defined(__linux__)
#include <pthread.h>
#include <sched.h>
#include <semaphore.h>
#include <cerrno>
#endif
namespace aeolus {
struct SpatialBatch::Impl {
    std::array<dsp::SpatialFilterPacket, Capacity> packets;
    Stats counters;
#if defined(__linux__)
    enum { queued, running, done, cancelled };
    struct alignas(64) Job {
        dsp::SpatialFilterPacket packet;
        dsp::SpatialFilterResult result;
        std::atomic<int> state {cancelled};
    };
    struct Worker {
        std::array<Job, Capacity/2> jobs;
        sem_t wake;
        bool initialized = false, active = false;
        int begin = 0, end = 0;
        std::thread thread;
        alignas(64) std::atomic<unsigned> requested {0}, finished {0};
    };
    std::array<Worker, 3> workers;
    const int count, firstCpu, testDelayUs;
    int voices = 0;
    std::atomic<int> ready {0};
    std::atomic<bool> stop {false}, failed {false};
    Impl(int count, int firstCpu, int testDelayUs)
        : count(count), firstCpu(firstCpu), testDelayUs(testDelayUs) {
        if (count < 0 || count > 3) throw std::runtime_error("spatial worker count must be 0..3");
        try {
            for (int lane = 0; lane < count; ++lane) {
                auto& w = workers[lane];
                if (sem_init(&w.wake, 0, 0) != 0) throw std::runtime_error("spatial sem_init failed");
                w.initialized = true;
                w.thread = std::thread([this, lane] { run(lane); });
            }
            while (ready.load(std::memory_order_acquire) != count) std::this_thread::yield();
            if (failed.load()) throw std::runtime_error("spatial worker setup failed");
        } catch (...) { shutdown(); throw; }
    }
    void run(int lane) noexcept {
        bool announced = false;
        try {
            const std::string name = "AeolusDSP" + std::to_string(lane+1);
            pthread_setname_np(pthread_self(), name.c_str());
            if (firstCpu >= 0) {
                const int cpu = firstCpu+lane+1;
                if (cpu >= CPU_SETSIZE) throw std::runtime_error("spatial CPU outside affinity mask");
                cpu_set_t mask; CPU_ZERO(&mask); CPU_SET(cpu, &mask);
                if (pthread_setaffinity_np(pthread_self(), sizeof(mask), &mask) != 0)
                    throw std::runtime_error("spatial worker affinity failed");
            }
            juce::ScopedNoDenormals noDenormals;
            auto& w = workers[lane];
            ready.fetch_add(1, std::memory_order_release); announced = true;
            for (;;) {
                int error;
                do { error = sem_wait(&w.wake); } while (error != 0 && errno == EINTR);
                if (error != 0) throw std::runtime_error("spatial sem_wait failed");
                if (stop.load(std::memory_order_acquire)) return;
                const auto epoch = w.requested.load(std::memory_order_acquire);
                if (testDelayUs && lane == 0 && epoch % 7 == 1)
                    std::this_thread::sleep_for(std::chrono::microseconds(testDelayUs));
                for (int i = 0; i < w.end-w.begin; ++i) {
                    auto& job = w.jobs[i]; int expected = queued;
                    if (job.state.compare_exchange_strong(expected, running, std::memory_order_acq_rel))
                        dsp::processSpatialFilterPacket(job.packet, job.result);
                    job.state.store(done, std::memory_order_release);
                }
                w.finished.store(epoch, std::memory_order_release);
            }
        } catch (...) {
            failed.store(true, std::memory_order_release);
            if (!announced) ready.fetch_add(1, std::memory_order_release);
        }
    }
    void shutdown() noexcept {
        stop.store(true, std::memory_order_release);
        for (auto& w : workers) if (w.initialized) sem_post(&w.wake);
        for (auto& w : workers) {
            if (w.thread.joinable()) w.thread.join();
            if (w.initialized) { sem_destroy(&w.wake); w.initialized = false; }
        }
    }
    ~Impl() {
        shutdown();
        std::clog << "Spatial batches: adopted=" << counters.adopted << " late=" << counters.late
                  << " local=" << counters.local << " busy_lanes=" << counters.busy << '\n';
    }
    Worker* laneFor(int index) noexcept {
        for (int lane = 0; lane < count; ++lane) {
            auto& w = workers[lane];
            // Use current batch boundaries, not potentially still-running old ones.
            if (index >= voices*(lane+1)/(count+1) && index < voices*(lane+2)/(count+1)) return &w;
        }
        return nullptr;
    }
#else
    Impl(int, int, int) { throw std::runtime_error("spatial batches require Linux"); }
#endif
};
SpatialBatch::SpatialBatch(int workers, int firstCpu, int testDelayUs)
    : impl(std::make_unique<Impl>(workers, firstCpu, testDelayUs)) {}
SpatialBatch::~SpatialBatch() = default;
std::unique_ptr<SpatialBatch> SpatialBatch::fromEnvironment() {
#if defined(__linux__) && defined(AEOLUS_EXPERIMENTAL_SPATIAL_BATCHES) && !AEOLUS_MULTIBUS_OUTPUT
    const auto* value = std::getenv("AEOLUS_SPATIAL_WORKERS");
    if (!value || value[0] == '\0' || value[1] != '\0' || value[0] < '0' || value[0] > '3') return {};
    const auto* affinity = std::getenv("AEOLUS_PERFORMANCE_CPU_BASE");
    const int firstCpu = affinity && std::string(affinity) == "0" ? 0 : -1;
    try { return std::make_unique<SpatialBatch>(value[0]-'0', firstCpu); }
    catch (const std::exception& e) { std::clog << "Spatial batches disabled: " << e.what() << '\n'; }
#endif
    return {};
}
void SpatialBatch::begin(int voices) noexcept {
#if defined(__linux__)
    jassert(voices > 0 && voices <= Capacity);
    impl->voices = voices;
    for (int lane = 0; lane < impl->count; ++lane) {
        auto& w = impl->workers[lane];
        w.active = !impl->failed.load(std::memory_order_acquire)
            && w.finished.load(std::memory_order_acquire) == w.requested.load(std::memory_order_relaxed);
        if (!w.active) { ++impl->counters.busy; continue; }
        w.begin = voices*(lane+1)/(impl->count+1); w.end = voices*(lane+2)/(impl->count+1);
        for (int j = 0; j < w.end-w.begin; ++j) w.jobs[j].state.store(Impl::cancelled, std::memory_order_relaxed);
    }
#else
    juce::ignoreUnused(voices);
#endif
}
dsp::SpatialFilterPacket& SpatialBatch::packet(int index) noexcept { return impl->packets[index]; }
void SpatialBatch::submit(int index) noexcept {
#if defined(__linux__)
    if (auto* w = impl->laneFor(index); w && w->active) {
        auto& job = w->jobs[index-w->begin];
        job.packet = impl->packets[index];
        job.state.store(Impl::queued, std::memory_order_relaxed);
    }
#else
    juce::ignoreUnused(index);
#endif
}
void SpatialBatch::dispatch() noexcept {
#if defined(__linux__)
    for (int lane = 0; lane < impl->count; ++lane) {
        auto& w = impl->workers[lane];
        if (!w.active) continue;
        w.requested.fetch_add(1, std::memory_order_release);
        // At most one outstanding post per lane; failure disables offload,
        // keeping canonical rendering available without waiting or stale output.
        if (sem_post(&w.wake) != 0) { w.active = false; impl->failed.store(true); }
    }
#endif
}
const dsp::SpatialFilterResult* SpatialBatch::result(int index) noexcept {
#if defined(__linux__)
    auto* w = impl->laneFor(index);
    if (!w) { ++impl->counters.local; return nullptr; }
    if (w->active) {
        auto& job = w->jobs[index-w->begin];
        if (job.state.load(std::memory_order_acquire) == Impl::done) {
            ++impl->counters.adopted; return &job.result;
        }
        int expected = Impl::queued;
        job.state.compare_exchange_strong(expected, Impl::cancelled, std::memory_order_acq_rel);
    }
    ++impl->counters.late;
#else
    juce::ignoreUnused(index);
#endif
    return nullptr;
}
SpatialBatch::Stats SpatialBatch::stats() const noexcept { return impl->counters; }
}
