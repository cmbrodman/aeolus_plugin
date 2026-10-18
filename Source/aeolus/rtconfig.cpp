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

#include "aeolus/rtconfig.h"

#include <cstdlib>
#include <cstring>

#if JUCE_LINUX
#include <pthread.h>
#include <sched.h>
#endif

AEOLUS_NAMESPACE_BEGIN

namespace {

bool readInt(const char* name, int& value)
{
    const char* text = std::getenv(name);

    if (text == nullptr || *text == '\0')
        return false;

    char* end = nullptr;
    const long v = std::strtol(text, &end, 10);

    if (end == text || *end != '\0')
        return false;

    value = (int) v;
    return true;
}

} // namespace

std::vector<int> RealtimeConfig::parseCpuList(const char* text)
{
    std::vector<int> cpus;

    if (text == nullptr)
        return cpus;

    const char* p = text;

    while (*p != '\0') {
        char* end = nullptr;
        const long first = std::strtol(p, &end, 10);

        if (end == p || first < 0 || first > 1023)
            return {};

        long last = first;
        p = end;

        if (*p == '-') {
            ++p;
            last = std::strtol(p, &end, 10);

            if (end == p || last < first || last > 1023)
                return {};

            p = end;
        }

        for (long cpu = first; cpu <= last; ++cpu)
            cpus.push_back((int) cpu);

        if (*p == ',')
            ++p;
        else if (*p != '\0')
            return {};
    }

    return cpus;
}

RealtimeConfig RealtimeConfig::fromEnvironment()
{
    RealtimeConfig config;
    int value = 0;

    if (readInt("AEOLUS_AUDIO_RT_PRIORITY", value))
        config.audioPriority = juce::jlimit(0, 99, value);

    config.audioCpus = parseCpuList(std::getenv("AEOLUS_AUDIO_CPUS"));

#if ! AEOLUS_MULTIBUS_OUTPUT
    // The batch renderer supports the stereo engine only.
    if (readInt("AEOLUS_DSP_WORKERS", value))
        config.dspWorkers = juce::jlimit(0, MaxDspWorkers, value);
#endif

    config.workerCpus = parseCpuList(std::getenv("AEOLUS_DSP_WORKER_CPUS"));

    if (readInt("AEOLUS_DSP_GROUP_SIZE", value))
        config.dspGroupSize = juce::jlimit(1, 64, value);

    if (readInt("AEOLUS_DSP_BATCH_QUANTA", value))
        config.maxBatchQuanta = juce::jlimit(1, 32, value);

    if (readInt("AEOLUS_MAX_VOICES", value))
        config.maxVoices = juce::jlimit(16, MaxVoicesLimit, value);

    if (readInt("AEOLUS_PER_VOICE_RANDOM", value))
        config.perVoiceRandom = value != 0;

    // Shared noise generators cannot be used from several threads, and the
    // batch renderer changes the order in which voices draw from them.
    if (config.isBatchRendering())
        config.perVoiceRandom = true;

    return config;
}

namespace rt {

ThreadPolicyStatus applyToCurrentThread(int priority, const std::vector<int>& cpus)
{
    ThreadPolicyStatus status;

#if JUCE_LINUX
    if (!cpus.empty()) {
        cpu_set_t set;
        CPU_ZERO(&set);

        for (int cpu : cpus)
            if (cpu >= 0 && cpu < CPU_SETSIZE)
                CPU_SET(cpu, &set);

        status.affinityError = pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
    }

    if (priority > 0) {
        sched_param param{};
        param.sched_priority = juce::jlimit(sched_get_priority_min(SCHED_FIFO),
                                            sched_get_priority_max(SCHED_FIFO),
                                            priority);
        status.priorityError = pthread_setschedparam(pthread_self(), SCHED_FIFO, &param);
    }
#else
    juce::ignoreUnused(priority, cpus);
#endif

    return status;
}

void setCurrentThreadName(const char* name)
{
#if JUCE_LINUX
    pthread_setname_np(pthread_self(), name);
#else
    juce::ignoreUnused(name);
#endif
}

AudioThreadReport& audioThreadReport()
{
    static AudioThreadReport report;
    return report;
}

void applyAudioThreadPolicy(const RealtimeConfig& config)
{
    // Device restarts may create a new audio thread; apply once per thread.
    static thread_local bool applied = false;

    if (applied)
        return;

    applied = true;

    if (config.audioPriority <= 0 && config.audioCpus.empty())
        return;

    // One-time system calls on this thread's first callback; nothing here
    // allocates or blocks on another thread.
    const auto status = applyToCurrentThread(config.audioPriority, config.audioCpus);
    auto& report = audioThreadReport();
    report.priorityError.store(status.priorityError, std::memory_order_relaxed);
    report.affinityError.store(status.affinityError, std::memory_order_relaxed);
    report.generation.fetch_add(1, std::memory_order_release);
}

juce::String describe(const ThreadPolicyStatus& status, int priority, const std::vector<int>& cpus)
{
    juce::String text;

    if (status.priorityError < 0)
        text << "scheduling unchanged";
    else if (status.priorityError == 0)
        text << "SCHED_FIFO " << priority;
    else
        text << "SCHED_FIFO " << priority << " refused (" << std::strerror(status.priorityError)
             << "; check the rtprio limit)";

    text << ", ";

    if (status.affinityError < 0) {
        text << "affinity unchanged";
    } else {
        juce::StringArray list;

        for (int cpu : cpus)
            list.add(juce::String(cpu));

        text << "CPUs " << list.joinIntoString(",");

        if (status.affinityError != 0)
            text << " refused (" << std::strerror(status.affinityError) << ")";
    }

    return text;
}

} // namespace rt

AEOLUS_NAMESPACE_END
