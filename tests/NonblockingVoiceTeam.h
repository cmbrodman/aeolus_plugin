// Copyright (C) 2026 Aeolus fork contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "aeolus/voicesnapshot.h"
#include "ProbeHeap.h"
#include "MulticoreProbe.h"
#include <semaphore.h>
#include <cerrno>

namespace voice_probe {
// Fixed immutable Pipewave fixture only. Canonical jobs and their resources must
// outlive the team. No completion wait in run(); only destruction joins workers.
// Each worker has at most one outstanding wake and owns private snapshot memory.
template<class Job> class NonblockingTeam final {
    enum State { cancelled, queued, running, done };
    struct Slot {
        int index;
        aeolus::VoiceSnapshot snapshot;
        alignas(64) std::atomic<int> state {cancelled};
        Slot(int index, const Job& job) : index(index), snapshot(*job.voice) {}
    };
    struct Worker {
        std::vector<std::unique_ptr<Slot>> slots;
        sem_t wake;
        bool initialized = false, active = false;
        std::thread thread;
        alignas(64) std::atomic<unsigned> requested {0}, finished {0};
    };
    std::vector<Job>& jobs;
    std::array<Worker,3> workers;
    const int count, delayUs;
    const std::atomic<bool>* const hold;
    std::atomic<int> ready {0};
    std::atomic<bool> stop {false}, failed {false};
    void shutdown() noexcept {
        stop.store(true, std::memory_order_release);
        for (auto& w : workers) if (w.initialized) sem_post(&w.wake);
        for (auto& w : workers) {
            if (w.thread.joinable()) w.thread.join();
            if (w.initialized) { sem_destroy(&w.wake); w.initialized = false; }
        }
    }
public:
    struct Stats { size_t adopted=0, fallback=0, local=0, busy=0, rejected=0; };
    Stats stats;
    NonblockingTeam(std::vector<Job>& jobs, int count, int firstCpu,
                    int delayUs=0, const std::atomic<bool>* hold=nullptr)
        : jobs(jobs), count(count), delayUs(delayUs), hold(hold) {
        if (count<1 || count>4 || jobs.size()>512) throw std::runtime_error("invalid snapshot team size");
        static_assert(std::atomic<int>::is_always_lock_free && std::atomic<unsigned>::is_always_lock_free
                      && std::atomic<bool>::is_always_lock_free,
                      "snapshot publication must be lock-free");
        try {
            for (int lane=1; lane<count; ++lane) {
                auto& w=workers[lane-1];
                for (int v=lane; v<int(jobs.size()); v+=count)
                    w.slots.push_back(std::make_unique<Slot>(v,jobs[v]));
                if (sem_init(&w.wake,0,0)!=0) throw std::runtime_error("voice snapshot sem_init failed");
                w.initialized=true;
                w.thread=std::thread([this,lane,firstCpu] {
                    bool announced=false;
                    try {
                        multicore_probe::Affinity affinity(firstCpu<0 ? -1 : firstCpu+lane);
                        juce::ScopedNoDenormals noDenormals;
                        auto& w=workers[lane-1];
                        ready.fetch_add(1,std::memory_order_release); announced=true;
                        for (;;) {
                            int error;
                            do { error=sem_wait(&w.wake); } while (error && errno==EINTR);
                            if (error) throw std::runtime_error("voice snapshot sem_wait failed");
                            if (stop.load(std::memory_order_acquire)) return;
                            const auto current=w.requested.load(std::memory_order_acquire);
                            // Fault injection runs exclusively on workers. The
                            // coordinator must progress while a worker is held.
                            while (this->hold && this->hold->load(std::memory_order_acquire)) {
                                if (stop.load(std::memory_order_acquire)) return;
                                std::this_thread::sleep_for(std::chrono::microseconds(100));
                            }
                            if (this->delayUs && current%17==1)
                                std::this_thread::sleep_for(std::chrono::microseconds(this->delayUs));
                            for (auto& slot : w.slots) {
                                int expected=queued;
                                if (slot->state.compare_exchange_strong(expected,running,std::memory_order_acq_rel)) {
                                    { probe_heap::Guard heapGuard; slot->snapshot.render(); }
                                    slot->state.store(done,std::memory_order_release);
                                }
                            }
                            w.finished.store(current,std::memory_order_release);
                        }
                    } catch (...) {
                        failed.store(true,std::memory_order_release);
                        if (!announced) ready.fetch_add(1,std::memory_order_release);
                    }
                });
            }
            while (ready.load(std::memory_order_acquire)!=count-1) std::this_thread::yield();
            if (failed.load(std::memory_order_acquire)) throw std::runtime_error("voice snapshot worker setup failed");
        } catch (...) { shutdown(); throw; }
    }
    ~NonblockingTeam() { shutdown(); }
    void clearStats() noexcept { stats={}; }
    template<size_t N> void run(std::array<float,N>& mix, unsigned quanta=1) noexcept {
        static_assert(N>=128 && N<=1024);
        if (!quanta || quanta>8 || quanta*128>N) { mix.fill(0); return; }
        probe_heap::Guard heapGuard;
        // Reuse a lane only after its entire previous request completed. A late
        // lane stays untouched; canonical voices continue locally meanwhile.
        for (int lane=1; lane<count; ++lane) {
            auto& w=workers[lane-1];
            w.active=!failed.load(std::memory_order_acquire)
                && w.finished.load(std::memory_order_acquire)==w.requested.load(std::memory_order_relaxed);
            if (!w.active) { ++stats.busy; continue; }
            for (auto& slot : w.slots) {
                auto& job=jobs[slot->index];
                const bool valid=slot->snapshot.capture(*job.voice,job.pipeRandom,job.chiffRandom,quanta);
                slot->state.store(valid ? queued : cancelled,std::memory_order_relaxed);
                if (!valid) ++stats.rejected;
            }
            w.requested.fetch_add(1,std::memory_order_release);
            if (sem_post(&w.wake)!=0) {
                failed.store(true,std::memory_order_release); w.active=false;
            }
        }
        for (int v=0; v<int(jobs.size()); v+=count) { jobs[v].render(quanta); ++stats.local; }
        // Consume in original voice order. A single acquire/claim attempt is
        // enough: never poll for a running job, wait, join or reuse its memory.
        for (int v=0; v<int(jobs.size()); ++v) {
            const int lane=v%count;
            if (!lane) continue;
            auto& w=workers[lane-1];
            auto& job=jobs[v];
            if (w.active) {
                auto& slot=*w.slots[v/count];
                if (slot.state.load(std::memory_order_acquire)==done) {
                    slot.snapshot.adopt(*job.voice,job.pipeRandom,job.chiffRandom);
                    std::copy_n(slot.snapshot.output().begin(),quanta*128,job.output.begin()); ++stats.adopted;
                    continue;
                }
                int expected=queued;
                slot.state.compare_exchange_strong(expected,cancelled,std::memory_order_acq_rel);
            }
            job.render(quanta); ++stats.fallback;
        }
        mix.fill(0);
        for (const auto& job : jobs)
            for (size_t i=0; i<quanta*128; ++i) mix[i]+=job.output[i];
    }
};
}
