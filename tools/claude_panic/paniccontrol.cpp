// Copyright (C) 2026 Aeolus fork contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "aeolus/paniccontrol.h"
#include "aeolus/engine.h"
#include <atomic>
#include <array>
#include <thread>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#if JUCE_LINUX
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <unistd.h>
#include <poll.h>
#include <cerrno>
#if JucePlugin_Build_Standalone
#include <juce_audio_plugin_client/Standalone/juce_StandaloneFilterWindow.h>
#endif
#endif

AEOLUS_NAMESPACE_BEGIN
struct PanicControl::Impl {
#if JUCE_LINUX
    Engine& engine;
    std::string path;
    int fd = -1;
    std::thread thread;
    std::atomic<bool> stopping {false};
    std::shared_ptr<std::atomic<bool>> alive = std::make_shared<std::atomic<bool>>(true);
    std::shared_ptr<std::atomic<bool>> preparing = std::make_shared<std::atomic<bool>>(false);
    dev_t device = 0;
    ino_t inode = 0;
    // Control-thread-owned bounded cache makes a retried request idempotent.
    std::array<std::array<char, 33>, 32> acceptedNonces {};
    size_t nextNonce = 0;
    Impl(Engine& owner, const char* socketPath) : engine(owner), path(socketPath) {
        sockaddr_un address {};
        if (path.empty() || path.front() != '/' || path.size() >= sizeof(address.sun_path))
            throw std::runtime_error("Panic socket must have a short absolute path");
        fd = ::socket(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (fd < 0) throw std::runtime_error("Cannot create panic control socket");
        address.sun_family = AF_UNIX;
        std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
        if (::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
            ::close(fd); fd = -1;
            throw std::runtime_error("Cannot bind panic control socket; existing paths are preserved");
        }
        ::chmod(path.c_str(), 0600);
        struct stat info {};
        if (::lstat(path.c_str(), &info) == 0) { device = info.st_dev; inode = info.st_ino; }
        try { thread = std::thread([this] { run(); }); }
        catch (...) { ::close(fd); ::unlink(path.c_str()); throw; }
    }
    ~Impl() {
        alive->store(false, std::memory_order_release);
        stopping.store(true, std::memory_order_release);
        if (thread.joinable()) thread.join();
        if (fd >= 0) ::close(fd);
        struct stat info {};
        if (::lstat(path.c_str(), &info) == 0 && info.st_dev == device && info.st_ino == inode)
            ::unlink(path.c_str());
    }
    void resumeOnMessageThread() {
        // Only explicit Resume restarts audio. Device stop/reprepare happens
        // outside the callback and clears old reverb/filter history safely.
        std::weak_ptr<std::atomic<bool>> weak = alive;
        if (preparing->exchange(true, std::memory_order_acq_rel)) return;
        const auto transition = preparing;
        const bool posted = juce::MessageManager::callAsync([weak, transition] {
            const auto guard = weak.lock();
            if (!guard || !guard->load(std::memory_order_acquire)) {
                transition->store(false, std::memory_order_release);
                return;
            }
#if JucePlugin_Build_Standalone
            if (auto* holder = juce::StandalonePluginHolder::getInstance()) {
                holder->deviceManager.closeAudioDevice();
                holder->deviceManager.restartLastAudioDevice();
            }
#endif
            transition->store(false, std::memory_order_release);
        });
        if (!posted) preparing->store(false, std::memory_order_release);
    }
    void run() noexcept {
        try {
            rt::setCurrentThreadName("aeolus-panic");
            const auto status = rt::applyToCurrentThread(1, {4});
            juce::Logger::writeToLog("Aeolus panic control: " + rt::describe(status, 1, {4}));
            while (!stopping.load(std::memory_order_acquire)) {
                pollfd ready {fd, POLLIN, 0};
                const int result = ::poll(&ready, 1, 50);
                if (result <= 0) continue;
                char command[64] {};
                sockaddr_un client {};
                socklen_t length = sizeof(client);
                const auto bytes = ::recvfrom(fd, command, sizeof(command) - 1, 0,
                                             reinterpret_cast<sockaddr*>(&client), &length);
                if (bytes <= 0) continue;
                command[bytes] = 0;
                if (std::strcmp(command, "HEALTH") == 0) {
                    // Read completion first so the following started snapshot
                    // includes at least the callbacks whose completion was seen.
                    const auto completed = engine.getAudioCompleted();
                    const auto started = engine.getAudioStarted();
                    char health[160] {};
                    const int count = std::snprintf(health, sizeof(health), "HEALTH 2 %llu %llu %u %llu",
                        (unsigned long long) started, (unsigned long long) completed,
                        engine.getAudioState(), (unsigned long long) engine.getGuiProgress());
                    ::sendto(fd, health, size_t(count), MSG_NOSIGNAL,
                             reinterpret_cast<sockaddr*>(&client), length);
                    continue;
                }
                bool panic = std::strncmp(command, "PANIC ", 6) == 0
                                   && bytes > 6 && bytes <= 38;
                if (panic) {
                    for (ssize_t i=6; i<bytes; ++i) {
                        const char c=command[i];
                        if (!((c>='0' && c<='9') || (c>='a' && c<='f') || (c>='A' && c<='F'))) panic=false;
                    }
                    if (!panic) continue;
                    bool accepted=false;
                    for (const auto& nonce : acceptedNonces)
                        if (std::strcmp(nonce.data(), command+6)==0) { accepted=true; break; }
                    if (!accepted) {
                        engine.requestEmergencyPanic();
                        std::memcpy(acceptedNonces[nextNonce].data(), command+6, size_t(bytes-6)+1);
                        nextNonce=(nextNonce+1)%acceptedNonces.size();
                    }
                }
                else if (std::strcmp(command, "RESUME") == 0) {
                    if (engine.isEmergencyMuted() && !engine.isEmergencyPanicPending()) resumeOnMessageThread();
                } else if (std::strcmp(command, "STATUS") != 0) continue;
                char reply[128] {};
                const auto acknowledged = engine.getPanicAcknowledged();
                const auto requested = engine.getPanicRequested();
                const int count = std::snprintf(reply, sizeof(reply), "STATUS %u %u %d%s%s",
                    requested, acknowledged, preparing->load(std::memory_order_acquire) ? 2 : int(engine.isEmergencyMuted()),
                    panic ? " " : "", panic ? command + 6 : "");
                ::sendto(fd, reply, size_t(count), MSG_NOSIGNAL,
                         reinterpret_cast<sockaddr*>(&client), length);
            }
        } catch (...) {
            // The independent supervisor will detect an unresponsive endpoint.
            // Never throw out of a thread or touch canonical voices here.
        }
    }
#else
    Impl(Engine&, const char*) {}
#endif
};

PanicControl::PanicControl(std::unique_ptr<Impl> implementation) : impl(std::move(implementation)) {}
PanicControl::~PanicControl() = default;
std::unique_ptr<PanicControl> PanicControl::create(Engine& engine) {
#if JUCE_LINUX
    if (const char* path = std::getenv("AEOLUS_PANIC_SOCKET")) {
        if (*path) return std::unique_ptr<PanicControl>(new PanicControl(std::make_unique<Impl>(engine, path)));
    }
#else
    juce::ignoreUnused(engine);
#endif
    return {};
}
AEOLUS_NAMESPACE_END
