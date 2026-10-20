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

#pragma once

#include "aeolus/globals.h"
#include "aeolus/rtconfig.h"

#include <atomic>
#include <memory>
#include <vector>

AEOLUS_NAMESPACE_BEGIN

/**
 * @brief Persistent worker threads that help the audio thread run a batch
 *        of independent jobs.
 *
 * run() publishes a batch, wakes sleeping workers, runs jobs itself and
 * returns once every job has completed. Jobs are claimed dynamically, so the
 * audio thread never waits for a job that no thread has started; it only
 * waits for jobs already running on a worker. That wait is bounded by one
 * job's duration as long as workers are not preempted, which is why workers
 * should run at the audio thread's real-time priority on CPUs of their own.
 * See docs/MULTICORE.md.
 *
 * Publication, claiming and wake-up do not allocate or take locks on Linux
 * (POSIX semaphores). Other platforms fall back to a mutex-based semaphore.
 */
class DspTeam final
{
public:
    using JobFunction = void (*)(void* context, int jobIndex);

    DspTeam(int numWorkers, const std::vector<int>& workerCpus, int priority,
            JobFunction function, void* context);
    ~DspTeam();

    DspTeam(const DspTeam&) = delete;
    DspTeam& operator=(const DspTeam&) = delete;

    int getNumWorkers() const noexcept { return (int) _workers.size(); }

    /// Run jobs [0, numJobs). Audio thread only; not reentrant.
    void run(int numJobs);

    struct Stats
    {
        uint64_t batches = 0;
        uint64_t jobs = 0;
        uint64_t jobsByWorkers = 0;
        uint64_t waits = 0;          ///< Batches where the caller had to wait.
        uint64_t waitNanosTotal = 0;
        uint64_t waitNanosMax = 0;
    };

    Stats getStats() const noexcept;

    /// Scheduling status of each worker, once all have started.
    juce::String describeWorkers() const;
    bool haveAllWorkersStarted() const noexcept;

    static constexpr int MaxJobs = 0xffff;

private:
    struct Worker;

    void workerLoop(Worker& worker, int index);
    bool claim(uint64_t& word, int& jobIndex) noexcept;
    void runClaimedJobs(uint64_t word, bool countForWorkers) noexcept;

    static uint64_t pack(uint32_t generation, int numJobs, int next) noexcept
    {
        return (uint64_t(generation) << 32) | (uint64_t(numJobs) << 16) | uint64_t(next);
    }

    static uint32_t generationOf(uint64_t w) noexcept { return uint32_t(w >> 32); }
    static int jobsOf(uint64_t w) noexcept { return int((w >> 16) & 0xffff); }
    static int nextOf(uint64_t w) noexcept { return int(w & 0xffff); }

    JobFunction _function;
    void* _context;
    int _priority;

    // Generation, job count and next unclaimed job in one word, so a stale
    // worker can never claim a job of a batch it has not observed.
    alignas(64) std::atomic<uint64_t> _claim { 0 };
    alignas(64) std::atomic<int> _done { 0 };
    alignas(64) std::atomic<bool> _running { true };

    uint32_t _generation = 0;

    std::atomic<uint64_t> _statBatches { 0 };
    std::atomic<uint64_t> _statJobs { 0 };
    std::atomic<uint64_t> _statJobsByWorkers { 0 };
    std::atomic<uint64_t> _statWaits { 0 };
    std::atomic<uint64_t> _statWaitNanosTotal { 0 };
    std::atomic<uint64_t> _statWaitNanosMax { 0 };

    std::vector<int> _cpus;
    std::vector<std::unique_ptr<Worker>> _workers;
};

AEOLUS_NAMESPACE_END
