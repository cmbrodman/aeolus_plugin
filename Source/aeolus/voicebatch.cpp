// Copyright (C) 2026 Aeolus fork contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "voicebatch.h"
#include "voicesnapshot.h"
#include "engine.h"
#include <algorithm>
#include <thread>
#include <cstdlib>
#include <cstdio>
#if defined(__linux__)
#include <semaphore.h>
#include <pthread.h>
#include <sched.h>
#include <cerrno>
namespace aeolus {
struct VoiceBatch::Impl {
    struct alignas(64) Job {
        Voice* voice;
        juce::Random pipeRandom, chiffRandom;
        alignas(64) std::array<float,1024> output {};
        bool active=false;
        unsigned overMask=0;
        Job(Voice& v, int index) : voice(&v), pipeRandom(1234567+index*17), chiffRandom(7654321+index*31) {}
        void render(unsigned quanta) noexcept {
            std::fill_n(output.begin(),quanta*128,0.0f); overMask=0;
            for (unsigned q=0; q<quanta; ++q) {
                if (q==0 || !voice->isOver()) voice->process(output.data()+q*128,output.data()+q*128+64,pipeRandom,chiffRandom);
                if (voice->isOver()) overMask|=1u<<q;
            }
        }
    };
    enum State { cancelled, queued, running, done };
    struct Slot {
        int index;
        aeolus::VoiceSnapshot snapshot;
        alignas(64) std::atomic<int> state {cancelled};
        Slot(int index, const Job& job) : index(index), snapshot(*job.voice,true) {}
    };
    struct Worker {
        std::vector<std::unique_ptr<Slot>> slots;
        sem_t wake;
        bool initialized = false, active = false;
        std::thread thread;
        alignas(64) std::atomic<unsigned> requested {0}, finished {0};
    };
    std::vector<Job> jobs;
    std::array<Worker,3> workers;
    VoicePool& pool;
    std::array<int,512> active {};
    int activeCount=0;
    bool allowLarge=true;
    unsigned quantum=0, quanta=0;
    const unsigned maxQuanta;
    const int count, delayUs;

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
    Impl(VoicePool& pool, int count, int firstCpu, int delayUs, unsigned maxQuanta)
        : pool(pool), maxQuanta(maxQuanta), count(count), delayUs(delayUs) {
        if (count<1 || count>4 || pool.capacity()>512 || !maxQuanta || maxQuanta>8
            || firstCpu>CPU_SETSIZE-4) throw std::runtime_error("invalid live voice team configuration");
        jobs.reserve(size_t(pool.capacity()));
        for (int i=0; i<pool.capacity(); ++i) jobs.emplace_back(pool.at(i),i);
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
                        if (firstCpu>=0) {
                            cpu_set_t mask; CPU_ZERO(&mask); CPU_SET(firstCpu+lane,&mask);
                            if (pthread_setaffinity_np(pthread_self(),sizeof(mask),&mask))
                                throw std::runtime_error("voice worker affinity failed");
                        }
                        const auto name=std::string("AeolusVoice")+std::to_string(lane);
                        pthread_setname_np(pthread_self(),name.c_str());
                        juce::ScopedNoDenormals noDenormals;
                        auto& w=workers[lane-1];
                        ready.fetch_add(1,std::memory_order_release); announced=true;
                        for (;;) {
                            int error;
                            do { error=sem_wait(&w.wake); } while (error && errno==EINTR);
                            if (error) throw std::runtime_error("voice snapshot sem_wait failed");
                            if (stop.load(std::memory_order_acquire)) return;
                            const auto current=w.requested.load(std::memory_order_acquire);
                            if (this->delayUs && current%17==1)
                                std::this_thread::sleep_for(std::chrono::microseconds(this->delayUs));
                            for (auto& slot : w.slots) {
                                int expected=queued;
                                if (slot->state.compare_exchange_strong(expected,running,std::memory_order_acq_rel)) {
                                    slot->snapshot.render();
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
    ~Impl() { shutdown(); }
    void clearStats() noexcept { stats={}; }
    void run(unsigned length) noexcept {
        quanta=allowLarge ? std::max(1u,std::min(length,maxQuanta)) : 1; quantum=0;
        const bool offload=quanta>=4 && activeCount>=64;
        // Reuse a lane only after its entire previous request completed. A late
        // lane stays untouched; canonical voices continue locally meanwhile.
        for (int lane=1; lane<count; ++lane) {
            auto& w=workers[lane-1];
            w.active=offload && !failed.load(std::memory_order_acquire)
                && w.finished.load(std::memory_order_acquire)==w.requested.load(std::memory_order_relaxed);
            if (!w.active) { if (offload) ++stats.busy; continue; }
            for (auto& slot : w.slots) {
                auto& job=jobs[slot->index];
                const bool valid=job.active && slot->snapshot.capture(*job.voice,job.pipeRandom,job.chiffRandom,quanta);
                slot->state.store(valid ? queued : cancelled,std::memory_order_relaxed);
                if (job.active && !valid) ++stats.rejected;
            }
            w.requested.fetch_add(1,std::memory_order_release);
            if (sem_post(&w.wake)!=0) {
                failed.store(true,std::memory_order_release); w.active=false;
            }
        }
        for (int i=0; i<activeCount; ++i) {
            auto& job=jobs[active[i]];
            if (active[i]%count==0) { job.render(quanta); ++stats.local; }
        }
        // Consume in original voice order. A single acquire/claim attempt is
        // enough: never poll for a running job, wait, join or reuse its memory.
        for (int i=0; i<activeCount; ++i) {
            const int v=active[i];
            const int lane=v%count;
            if (!lane) continue;
            auto& w=workers[lane-1];
            auto& job=jobs[v];
            if (w.active) {
                auto& slot=*w.slots[v/count];
                if (slot.state.load(std::memory_order_acquire)==done) {
                    slot.snapshot.adopt(*job.voice,job.pipeRandom,job.chiffRandom);
                    std::copy_n(slot.snapshot.output().begin(),quanta*128,job.output.begin());
                    job.overMask=slot.snapshot.overMask(); ++stats.adopted;
                    continue;
                }
                int expected=queued;
                slot.state.compare_exchange_strong(expected,cancelled,std::memory_order_acq_rel);
            }
            job.render(quanta); if (offload) ++stats.fallback; else ++stats.local;
        }
    }
};
std::unique_ptr<VoiceBatch> VoiceBatch::fromEnvironment(VoicePool& pool) {
#if AEOLUS_EXPERIMENTAL_VOICE_BATCHES
    const char* value=std::getenv("AEOLUS_VOICE_WORKERS");
    if (!value) return {};
    const std::string workers(value);
    if (workers!="0" && workers!="1" && workers!="2" && workers!="3")
        throw std::runtime_error("AEOLUS_VOICE_WORKERS must be 0..3");
    const char* cpu=std::getenv("AEOLUS_PERFORMANCE_CPU_BASE");
    const int first=cpu && std::string(cpu)=="0" ? 0 : -1;
    const char* delay=std::getenv("AEOLUS_VOICE_TEST_DELAY_US");
    const int delayUs=delay && std::string(delay)=="2000" ? 2000 : 0;
    const char* maximum=std::getenv("AEOLUS_VOICE_JOB_QUANTA");
    const unsigned quanta=maximum && std::string(maximum)=="1" ? 1 : 8;
    size_t history=SAMPLE_RATE;
    auto* global=EngineGlobal::getInstance();
    for (int i=0; i<global->getStopsCount(); ++i) {
        const auto& model=global->getStop(i)->getModel();
        const double x=0.15*model.getFd()/model.getFn()*65.0+0.15;
        const double frames=std::hypot(x,5.0)*SAMPLE_RATE_F/330.0+64;
        if (!std::isfinite(frames) || frames>16*SAMPLE_RATE_F)
            throw std::runtime_error("experimental spatial-history bound unsupported");
        history=std::max(history,size_t(frames));
    }
    for (int i=0; i<pool.capacity(); ++i) pool.at(i).reserveSpatialHistory(history);
    return std::make_unique<VoiceBatch>(pool,std::stoi(workers)+1,first,delayUs,quanta);
#else
    juce::ignoreUnused(pool); return {};
#endif
}
VoiceBatch::VoiceBatch(VoicePool& p,int n,int cpu,int delay,unsigned maximum)
    : impl(std::make_unique<Impl>(p,n,cpu,delay,maximum)) {}
VoiceBatch::~VoiceBatch() {
    const auto s=stats();
    std::fprintf(stderr,"AEOLUS_VOICE_BATCH adopted=%zu fallback=%zu local=%zu busy=%zu rejected=%zu\n",
                 s.adopted,s.fallback,s.local,s.busy,s.rejected);
}
void VoiceBatch::begin() noexcept { impl->allowLarge=true; impl->activeCount=0; for (auto& job:impl->jobs) job.active=false; }
void VoiceBatch::include(Voice* v) noexcept {
    const int index=impl->pool.indexOf(v);
    jassert(index>=0 && index<int(impl->jobs.size()) && !impl->jobs[index].active);
    if (!v->isActive()) impl->allowLarge=false;
    impl->jobs[index].active=true; impl->active[impl->activeCount++]=index;
}
void VoiceBatch::requireQuantum() noexcept { impl->allowLarge=false; }
void VoiceBatch::render(unsigned n) noexcept { impl->run(n); }
bool VoiceBatch::hasPrepared() const noexcept { return impl->quantum<impl->quanta; }
void VoiceBatch::copy(Voice* v,float* l,float* r) const noexcept {
    const auto& job=impl->jobs[impl->pool.indexOf(v)];
    const float* out=job.output.data()+impl->quantum*128;
    std::copy_n(out,64,l); std::copy_n(out+64,64,r);
}
bool VoiceBatch::isOver(Voice* v) const noexcept {
    return impl->jobs[impl->pool.indexOf(v)].overMask & (1u<<impl->quantum);
}
void VoiceBatch::advance() noexcept { ++impl->quantum; }
VoiceBatch::Stats VoiceBatch::stats() const noexcept {
    const auto& s=impl->stats; return {s.adopted,s.fallback,s.local,s.busy,s.rejected};
}
}
#else
namespace aeolus {
struct VoiceBatch::Impl {};
std::unique_ptr<VoiceBatch> VoiceBatch::fromEnvironment(VoicePool&) { return {}; }
VoiceBatch::VoiceBatch(VoicePool&,int,int,int,unsigned) { jassertfalse; }
VoiceBatch::~VoiceBatch() = default;
void VoiceBatch::begin() noexcept {}
void VoiceBatch::include(Voice*) noexcept {}
void VoiceBatch::requireQuantum() noexcept {}
void VoiceBatch::render(unsigned) noexcept {}
bool VoiceBatch::hasPrepared() const noexcept { return false; }
void VoiceBatch::copy(Voice*,float*,float*) const noexcept {}
bool VoiceBatch::isOver(Voice*) const noexcept { return false; }
void VoiceBatch::advance() noexcept {}
VoiceBatch::Stats VoiceBatch::stats() const noexcept { return {}; }
}
#endif
