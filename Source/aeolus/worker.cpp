// ----------------------------------------------------------------------------
//
//  Copyright (C) 2021 Arthur Benilov <arthur.benilov@gmail.com>
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

#include "aeolus/worker.h"
#include "aeolus/ringbuffer.h"
#if JUCE_WINDOWS
#include <windows.h>
#elif JUCE_MAC
#include <dispatch/dispatch.h>
#else
#include <semaphore.h>
#include <cerrno>
#endif

#include <atomic>
#include <mutex>
#include <thread>
#include <vector>
#include <stdexcept>

using namespace juce;

AEOLUS_NAMESPACE_BEGIN

struct Worker::Impl
{
    RingBuffer<Worker::Job*, DefaultCapacity> jobsQueue;

    struct Wakeup {
#if JUCE_WINDOWS
        HANDLE handle = CreateSemaphore(nullptr, 0, LONG_MAX, nullptr);
        Wakeup() { if (!handle) throw std::runtime_error("Cannot create worker semaphore"); }
        ~Wakeup() { CloseHandle(handle); }
        void notify() { ReleaseSemaphore(handle, 1, nullptr); }
        void wait() { WaitForSingleObject(handle, INFINITE); }
        bool tryWait() { return WaitForSingleObject(handle, 0) == WAIT_OBJECT_0; }
#elif JUCE_MAC
        dispatch_semaphore_t handle = dispatch_semaphore_create(0);
        Wakeup() { if (!handle) throw std::runtime_error("Cannot create worker semaphore"); }
        ~Wakeup() { dispatch_release(handle); }
        void notify() { dispatch_semaphore_signal(handle); }
        void wait() { dispatch_semaphore_wait(handle, DISPATCH_TIME_FOREVER); }
        bool tryWait() { return dispatch_semaphore_wait(handle, DISPATCH_TIME_NOW) == 0; }
#else
        sem_t handle;
        Wakeup() { const int result = sem_init(&handle, 0, 0); if (result != 0) throw std::runtime_error("Cannot create worker semaphore"); }
        ~Wakeup() { sem_destroy(&handle); }
        void notify() { sem_post(&handle); }
        void wait() { while (sem_wait(&handle) != 0 && errno == EINTR) {} }
        bool tryWait() { return sem_trywait(&handle) == 0; }
#endif
    } sema;
    std::atomic_bool running;
    std::unique_ptr<std::thread> thread;

    Impl()
        : jobsQueue()
        , sema()
        , running(false)
        , thread(nullptr)
    {
    }

    ~Impl()
    {
        stop();
    }

    void run()
    {
        while (running) {
            Worker::Job* job = nullptr;

            wait();

            if (running && jobsQueue.receive (job)) {
                jassert(job != nullptr);
                job->run();
            }
        }
    }

    bool addJob (Job* job)
    {
        jassert(job != nullptr);

        if (!running.load()) return false;
        const auto ok = jobsQueue.send (job);
        if (ok) wakeUp();

        return ok;
    }

    void start()
    {
        if (thread != nullptr) return;
        purge();

        if (thread == nullptr) {
            running = true;
            thread = std::make_unique<std::thread> (&Impl::run, this);
        }
    }

    void stop()
    {
        if (thread != nullptr) {
            running = false;
            wakeUp();

            if (thread->joinable())
                thread->join();
            thread.reset();
        }
        // No consumer remains. Queued raw job pointers must not survive setup
        // resetting or destroying their owners, or a subsequent worker restart.
        purge();
        while (sema.tryWait()) {}
    }

    bool hasPendingJobs() noexcept
    {
        return jobsQueue.count() > 0;
    }

    bool isRunning() const noexcept
    {
        return running;
    }

    void purge()
    {
        Worker::Job* job;

        while (jobsQueue.receive (job)) {
            if (job) job->cancelled();
        }
    }

    void wait()
    {
        sema.wait();
    }

    void wakeUp()
    {
        sema.notify();
    }
};

//----------------------------------------------------------

Worker::Worker()
    : d(std::make_unique<Impl>())
{
}

Worker::~Worker() = default;

void Worker::start()
{
    d->start();
}

void Worker::stop()
{
    d->stop();
}

bool Worker::addJob(Job* job)
{
    return d->addJob (job);
}

bool Worker::hasPendingJobs() noexcept
{
    return d->hasPendingJobs();
}

bool Worker::isRunning() const noexcept
{
    return d->isRunning();
}

void Worker::purge()
{
    jassert(!d->isRunning());
    if (d->isRunning()) return;
    d->purge();
}

AEOLUS_NAMESPACE_END
