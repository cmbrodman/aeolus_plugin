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

#include <atomic>
#include <vector>

AEOLUS_NAMESPACE_BEGIN

/**
 * @brief Real-time scheduling and rendering configuration.
 *
 * Read once from environment variables when an engine is constructed.
 * Every option defaults to the original behaviour; see docs/MULTICORE.md.
 */
struct RealtimeConfig
{
    /// AEOLUS_AUDIO_RT_PRIORITY: SCHED_FIFO priority for the audio callback
    /// thread and DSP workers (Linux). 0 leaves scheduling unchanged.
    int audioPriority = 0;

    /// AEOLUS_AUDIO_CPUS: CPUs the audio callback thread may run on.
    std::vector<int> audioCpus;

    /// AEOLUS_DSP_WORKERS: -1 (unset) keeps the original serial renderer.
    /// 0 or more selects the batch renderer with that many worker threads in
    /// addition to the audio thread.
    int dspWorkers = -1;

    /// AEOLUS_DSP_WORKER_CPUS: worker i is pinned to entry i (modulo size).
    std::vector<int> workerCpus;

    /// AEOLUS_DSP_GROUP_SIZE: voices per batch job (summation group).
    int dspGroupSize = 8;

    /// AEOLUS_DSP_BATCH_QUANTA: maximum 64-sample quanta per batch (1-32).
    int maxBatchQuanta = 32;

    /// AEOLUS_MAX_VOICES: voice pool size.
    int maxVoices = 512;

    /// AEOLUS_PER_VOICE_RANDOM: give every voice its own deterministic noise
    /// streams. Always enabled by the batch renderer.
    bool perVoiceRandom = false;

    bool isBatchRendering() const noexcept { return dspWorkers >= 0; }

    static RealtimeConfig fromEnvironment();

    /// Parse a CPU list such as "0", "1,2,3" or "1-3,6".
    static std::vector<int> parseCpuList(const char* text);

    static constexpr int MaxDspWorkers = 15;
    static constexpr int MaxVoicesLimit = 4096;
};

namespace rt {

/// Result of applying a thread policy. 0 = success, -1 = not requested,
/// otherwise an errno value.
struct ThreadPolicyStatus
{
    int priorityError = -1;
    int affinityError = -1;
};

/// Apply SCHED_FIFO priority (when > 0) and CPU affinity (when non-empty)
/// to the calling thread. Linux only; elsewhere both report "not requested".
ThreadPolicyStatus applyToCurrentThread(int priority, const std::vector<int>& cpus);

/// Name the calling thread (visible in ps/top on Linux).
void setCurrentThreadName(const char* name);

/// Apply the configured audio-thread policy once per OS thread. Cheap after
/// the first call on a thread, so it may be called from every callback.
void applyAudioThreadPolicy(const RealtimeConfig& config);
int audioThreadPriority() noexcept;

/// Describe a policy status for logs.
juce::String describe(const ThreadPolicyStatus& status, int priority, const std::vector<int>& cpus);

/// Audio-thread status published by applyAudioThreadPolicy().
struct AudioThreadReport
{
    std::atomic<int> generation { 0 };   ///< Incremented after each application.
    std::atomic<int> priorityError { -1 };
    std::atomic<int> affinityError { -1 };
};

AudioThreadReport& audioThreadReport();

/// Spin-wait hint for the current CPU.
inline void cpuRelax() noexcept
{
#if defined(__aarch64__) || defined(__arm__)
    __asm__ __volatile__("yield");
#elif defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#endif
}

} // namespace rt

AEOLUS_NAMESPACE_END
