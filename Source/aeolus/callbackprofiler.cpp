// Copyright (C) 2026 Aeolus fork contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "callbackprofiler.h"
#include <chrono>
#include <condition_variable>
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif
#include <filesystem>
#include <iomanip>
#include <locale>
#include <mutex>
#include <sstream>
#if defined(__linux__)
#include <pthread.h>
#endif

namespace aeolus {
struct CallbackProfiler::State {
    explicit State(std::string name) : path(std::move(name)) {}
    std::string path;
    RingBuffer<Sample, 4096> queue;
    std::atomic<uint64_t> dropped {0};
    std::atomic<bool> stopping {false}, error {false}, finished {false};
    std::mutex mutex;
    std::condition_variable wake;
#if AEOLUS_TESTING
    std::shared_ptr<WriterTestState> test;
#endif
};
std::unique_ptr<CallbackProfiler> CallbackProfiler::create(const juce::String& path)
{
    if (path.isEmpty() || !juce::File::isAbsolutePath(path)) return {};
    const juce::File file(path);
    if (file.exists() || file.getFileExtension() != ".csv") return {};
    return std::unique_ptr<CallbackProfiler>(new CallbackProfiler(
        std::make_shared<State>(path.toStdString())));
}
#if AEOLUS_TESTING
std::unique_ptr<CallbackProfiler> CallbackProfiler::createForTesting(
    const juce::String& path, std::shared_ptr<WriterTestState> test)
{
    auto state = std::make_shared<State>(path.toStdString());
    state->test = std::move(test);
    return std::unique_ptr<CallbackProfiler>(new CallbackProfiler(std::move(state)));
}
#endif
CallbackProfiler::CallbackProfiler(std::shared_ptr<State> state) : _state(std::move(state))
{
    // No lambda holds the profiler/processor: a blocked writer can safely outlive
    // their destruction. Only standard-library state remains on that thread.
    _writer = std::thread(&CallbackProfiler::writeRecords, _state);
}
CallbackProfiler::~CallbackProfiler()
{
    std::unique_lock<std::mutex> lock(_state->mutex);
    _state->stopping.store(true, std::memory_order_release);
    _state->wake.notify_all(); // producer already stopped; never called in record()
    const bool finished = _state->wake.wait_for(lock, std::chrono::milliseconds(100),
        [this] { return _state->finished.load(std::memory_order_acquire); });
    lock.unlock();
    if (finished) _writer.join();
    else _writer.detach(); // bounded shutdown even if storage is blocked
}
uint64_t CallbackProfiler::getDroppedCount() const noexcept
{
    return _state->dropped.load(std::memory_order_relaxed);
}
bool CallbackProfiler::hasWriteError() const noexcept
{
    return _state->error.load(std::memory_order_acquire);
}
void CallbackProfiler::record(int64_t start, int64_t duration, double rate,
                              int frames, int voices, int xruns) noexcept
{
    if (!_state->queue.send({_sequence++, start, duration, rate, frames, voices, xruns, attacktiming::snapshot()}))
        _state->dropped.fetch_add(1, std::memory_order_relaxed);
}
void CallbackProfiler::writeRecords(std::shared_ptr<State> state) noexcept
{
#if defined(__linux__)
    pthread_setname_np(pthread_self(), "aeolus-log");
#endif
    // Only this thread formats, opens, writes, flushes and closes the stream.
    // Unbuffered OS writes publish without fsync or a registered stdio stream
    // that libc might try to flush/wait for during process exit.
    int output = -1;
    try {
#if AEOLUS_TESTING
        const auto testPause = [&] {
            if (state->test) {
                state->test->entered.store(true, std::memory_order_release);
                while (state->test->paused.load(std::memory_order_acquire))
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        };
        testPause();
#endif
#if defined(_WIN32)
        output = _wopen(std::filesystem::u8path(state->path).c_str(),
                        _O_WRONLY | _O_CREAT | _O_EXCL | _O_BINARY | _O_NOINHERIT, _S_IREAD | _S_IWRITE);
#else
        output = ::open(state->path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
#endif
        if (output < 0) state->error.store(true, std::memory_order_release);
        const auto write = [&](const std::string& text) {
            size_t offset = 0;
            while (output >= 0 && !state->error.load(std::memory_order_relaxed) && offset < text.size()) {
#if defined(_WIN32)
                const auto written = _write(output, text.data()+offset, (unsigned) (text.size()-offset));
#else
                const auto written = ::write(output, text.data()+offset, text.size()-offset);
#endif
                if (written < 0 && errno == EINTR) continue;
                if (written <= 0) state->error.store(true, std::memory_order_release);
                else offset += size_t(written);
            }
        };
        write("sequence,start_ns,duration_ns,sample_rate,frames,voices,native_xruns"
#if defined(AEOLUS_ATTACK_TIMING)
              ",midi_ns,render_ns,chiff_setup_ns,trigger_ns,chiff_clear_ns,spatial_setup_ns,reserved_ns,sampled_pipe_ns,sampled_chiff_ns,sampled_spatial_ns,triggers,render_calls,voice_calls,sampled_voice_calls"
#endif
              "\n");
        for (;;) {
#if AEOLUS_TESTING
            testPause();
#endif
            std::ostringstream text;
            text.imbue(std::locale::classic());
            text << std::fixed << std::setprecision(3);
            const auto available = state->queue.count();
            Sample sample {};
            for (size_t i = 0; i < available && state->queue.receive(sample); ++i) {
                text << sample.sequence << ',' << sample.startNs << ',' << sample.durationNs << ','
                     << sample.sampleRate << ',' << sample.frames << ',' << sample.voices << ',' << sample.nativeXruns;
#if defined(AEOLUS_ATTACK_TIMING)
                for (const auto ns : sample.attack.ns) text << ',' << ns;
                text << ',' << sample.attack.triggers << ',' << sample.attack.renderCalls
                     << ',' << sample.attack.voiceCalls << ',' << sample.attack.sampledVoiceCalls;
#endif
                text << '\n';
            }
            if (available != 0) write(text.str());
            if (state->stopping.load(std::memory_order_acquire) && state->queue.count() == 0) break;
            std::unique_lock<std::mutex> lock(state->mutex);
            state->wake.wait_for(lock, std::chrono::milliseconds(200),
                                [&] { return state->stopping.load(std::memory_order_acquire); });
        }
        write("# dropped_total=" + std::to_string(state->dropped.load(std::memory_order_relaxed)) + "\n");
    } catch (...) {
        state->error.store(true, std::memory_order_release);
    }
    if (output >= 0) {
#if defined(_WIN32)
        const int result = _close(output);
#else
        const int result = ::close(output);
#endif
        if (result != 0) state->error.store(true, std::memory_order_release);
    }
#if AEOLUS_TESTING
    if (state->test) state->test->finished.store(true, std::memory_order_release);
#endif
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->finished.store(true, std::memory_order_release);
    }
    state->wake.notify_all();
}
}
