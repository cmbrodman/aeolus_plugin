// ----------------------------------------------------------------------------
//
//  Copyright (C) 2026 Aeolus fork contributors
//
//  This program is free software; you can redistribute it and/or modify
//  it under the terms of the GNU General Public License as published by
//  the Free Software Foundation; either version 3 of the License, or
//  (at your option) any later version.
//
//  This program is distributed in the hope that it will be useful,
//  but WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//  GNU General Public License for more details.
//
//  You should have received a copy of the GNU General Public License
//  along with this program.  If not, see <http://www.gnu.org/licenses/>.
//
// ----------------------------------------------------------------------------

#include "aeolus/dspteam.h"

#include <chrono>
#include <thread>
#include <system_error>

#if JUCE_LINUX
#include <semaphore.h>
#include <cerrno>
#else
#include "aeolus/sema.h"
#endif

AEOLUS_NAMESPACE_BEGIN

namespace {

/// Wake-up signal posted from the audio thread.
class WakeSignal final
{
public:
#if JUCE_LINUX
    WakeSignal() { if(sem_init(&_sem, 0, 0)!=0) throw std::system_error(errno,std::generic_category(),"DSP semaphore"); }
    ~WakeSignal() { sem_destroy(&_sem); }
    void post() noexcept { sem_post(&_sem); }

    void wait() noexcept
    {
        while (sem_wait(&_sem) != 0 && errno == EINTR) {}
    }

private:
    sem_t _sem;
#else
    void post() { _sem.notify(); }
    void wait() { _sem.wait(); }

private:
    Semaphore _sem;
#endif
};

/// Spin this long before yielding the CPU while waiting for workers.
constexpr auto spinBeforeYield = std::chrono::microseconds(50);

} // namespace

struct DspTeam::Worker
{
    std::thread thread;
    WakeSignal signal;
    std::vector<int> cpus;
    alignas(64) std::atomic<bool> sleeping { false };
    std::atomic<bool> started { false };
    std::atomic<int> actualPriority { 0 };
    std::atomic<int> priorityError { -1 };
    std::atomic<int> affinityError { -1 };
};

DspTeam::DspTeam(int numWorkers, const std::vector<int>& workerCpus, int priority,
                 JobFunction function, void* context)
    : _function{ function }
    , _context{ context }
    , _priority{ priority }
    , _cpus{ workerCpus }
{
    jassert(function != nullptr);

    for (int i = 0; i < numWorkers; ++i) {
        auto worker = std::make_unique<Worker>();

        if (!workerCpus.empty())
            worker->cpus.push_back(workerCpus[(size_t) i % workerCpus.size()]);

        _workers.push_back(std::move(worker));
    }

    try {
        for (int i = 0; i < numWorkers; ++i) {
            auto& worker = *_workers[(size_t) i];
            worker.thread = std::thread([this, &worker, i] { workerLoop(worker, i); });
        }
    } catch(...) {
        _running.store(false,std::memory_order_seq_cst);
        for(auto& worker:_workers) worker->signal.post();
        for(auto& worker:_workers) if(worker->thread.joinable()) worker->thread.join();
        throw;
    }
}

DspTeam::~DspTeam()
{
    _running.store(false, std::memory_order_seq_cst);

    for (auto& worker : _workers)
        worker->signal.post();

    for (auto& worker : _workers)
        if (worker->thread.joinable())
            worker->thread.join();
}

void DspTeam::workerLoop(Worker& worker, int index)
{
    // Match the audio thread's floating-point mode so that results do not
    // depend on which thread renders a job.
    juce::ScopedNoDenormals noDenormals;

    const auto name = "aeolus-dsp-" + std::to_string(index + 1);
    rt::setCurrentThreadName(name.c_str());

    const auto status = rt::applyToCurrentThread(_priority, worker.cpus);
    worker.priorityError.store(status.priorityError, std::memory_order_relaxed);
    worker.affinityError.store(status.affinityError, std::memory_order_relaxed);
#if JUCE_LINUX
    int policy=0;sched_param parameter {};
    if(pthread_getschedparam(pthread_self(),&policy,&parameter)==0 && (policy==SCHED_FIFO || policy==SCHED_RR))
        worker.actualPriority.store(parameter.sched_priority,std::memory_order_relaxed);
#endif
    worker.started.store(true, std::memory_order_release);

    uint32_t seen = 0;

    while (_running.load(std::memory_order_acquire)) {
        uint64_t word = _claim.load(std::memory_order_acquire);

        if (generationOf(word) == seen) {
            // Announce sleep before re-checking, so that either this thread
            // observes the next batch or the publisher observes the flag.
            worker.sleeping.store(true, std::memory_order_seq_cst);
            word = _claim.load(std::memory_order_seq_cst);

            if (generationOf(word) == seen && _running.load(std::memory_order_seq_cst))
                worker.signal.wait();

            worker.sleeping.store(false, std::memory_order_relaxed);
            continue;
        }

        seen = generationOf(word);
        runClaimedJobs(word, true);
    }
}

bool DspTeam::claim(uint64_t& word, int& jobIndex) noexcept
{
    const uint32_t generation = generationOf(word);
    uint64_t w = _claim.load(std::memory_order_acquire);

    for (;;) {
        if (generationOf(w) != generation || nextOf(w) >= jobsOf(w))
            return false;

        if (_claim.compare_exchange_weak(w, w + 1, std::memory_order_acq_rel, std::memory_order_acquire)) {
            jobIndex = nextOf(w);
            word = w + 1;
            return true;
        }
    }
}

void DspTeam::runClaimedJobs(uint64_t word, bool countForWorkers) noexcept
{
    int jobIndex = 0;
    uint64_t count = 0;

    while (claim(word, jobIndex)) {
        _function(_context, jobIndex);
        _done.fetch_add(1, std::memory_order_acq_rel);
        ++count;
    }

    if (countForWorkers && count > 0)
        _statJobsByWorkers.fetch_add(count, std::memory_order_relaxed);
}

void DspTeam::run(int numJobs)
{
    if (numJobs <= 0)
        return;

    jassert(numJobs <= MaxJobs);

    _statBatches.store(_statBatches.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
    _statJobs.store(_statJobs.load(std::memory_order_relaxed) + uint64_t(numJobs), std::memory_order_relaxed);

    bool dispatch=!_workers.empty();
    const int callerPriority=rt::audioThreadPriority();
    for(const auto& worker:_workers) {
        if(!worker->started.load(std::memory_order_acquire)) { dispatch=false;break; }
        if(worker->priorityError.load(std::memory_order_relaxed)>0 || worker->affinityError.load(std::memory_order_relaxed)>0 ||
           worker->actualPriority.load(std::memory_order_relaxed)<callerPriority) { dispatch=false;break; }
    }
    // Fall back before publishing a generation. No live worker job or canonical
    // state is abandoned if scheduling setup is unavailable or incompatible.
    if (!dispatch) {
        for (int i = 0; i < numJobs; ++i)
            _function(_context, i);

        return;
    }

    _done.store(0, std::memory_order_relaxed);

    if (++_generation == 0)
        ++_generation;

    const uint64_t word = pack(_generation, numJobs, 0);

    // Job inputs written before this store are visible to any thread that
    // claims a job of this generation.
    _claim.store(word, std::memory_order_seq_cst);

    for (auto& worker : _workers)
        if (worker->sleeping.exchange(false, std::memory_order_seq_cst))
            worker->signal.post();

    runClaimedJobs(word, false);

    if (_done.load(std::memory_order_acquire) >= numJobs)
        return;

    // Every job has been claimed; wait for those still running on workers.
    using Clock = std::chrono::steady_clock;
    const auto start = Clock::now();
    auto now = start;
    unsigned spins = 0;

    while (_done.load(std::memory_order_acquire) < numJobs) {
        if ((++spins & 63) == 0)
            now = Clock::now();

        // Yield only shares execution with equal-priority FIFO peers. The
        // pre-publication check excludes lower-priority worker dispatch.
        if (now - start < spinBeforeYield)
            rt::cpuRelax();
        else
            std::this_thread::yield();
    }

    const auto waited = (uint64_t) std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count();
    _statWaits.store(_statWaits.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
    _statWaitNanosTotal.store(_statWaitNanosTotal.load(std::memory_order_relaxed) + waited, std::memory_order_relaxed);

    if (waited > _statWaitNanosMax.load(std::memory_order_relaxed))
        _statWaitNanosMax.store(waited, std::memory_order_relaxed);
}

DspTeam::Stats DspTeam::getStats() const noexcept
{
    Stats stats;
    stats.batches = _statBatches.load(std::memory_order_relaxed);
    stats.jobs = _statJobs.load(std::memory_order_relaxed);
    stats.jobsByWorkers = _statJobsByWorkers.load(std::memory_order_relaxed);
    stats.waits = _statWaits.load(std::memory_order_relaxed);
    stats.waitNanosTotal = _statWaitNanosTotal.load(std::memory_order_relaxed);
    stats.waitNanosMax = _statWaitNanosMax.load(std::memory_order_relaxed);
    return stats;
}

bool DspTeam::haveAllWorkersStarted() const noexcept
{
    for (const auto& worker : _workers)
        if (!worker->started.load(std::memory_order_acquire))
            return false;

    return true;
}

juce::String DspTeam::describeWorkers() const
{
    juce::StringArray lines;

    for (size_t i = 0; i < _workers.size(); ++i) {
        const auto& worker = *_workers[i];

        if (!worker.started.load(std::memory_order_acquire)) {
            lines.add("worker " + juce::String((int) i + 1) + ": not started");
            continue;
        }

        rt::ThreadPolicyStatus status;
        status.priorityError = worker.priorityError.load(std::memory_order_relaxed);
        status.affinityError = worker.affinityError.load(std::memory_order_relaxed);
        lines.add("worker " + juce::String((int) i + 1) + ": " + rt::describe(status, _priority, worker.cpus));
    }

    return lines.joinIntoString("; ");
}

AEOLUS_NAMESPACE_END
