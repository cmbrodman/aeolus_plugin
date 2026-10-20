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
#include <algorithm>
#ifndef AEOLUS_SAFETY_PYTHON
#define AEOLUS_SAFETY_PYTHON "python3"
#endif
#if JUCE_LINUX
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <unistd.h>
#include <poll.h>
#include <cerrno>
#include <spawn.h>
#include <sys/wait.h>
#include <fcntl.h>
extern char **environ;
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
    std::string runtime;
    pid_t watcher = -1;
    std::vector<int> controlCpus;
    int controlPriority = 0;
    bool ownedRuntime = false;

    std::thread thread;
    std::atomic<bool> stopping {false};
    std::shared_ptr<std::atomic<bool>> alive = std::make_shared<std::atomic<bool>>(true);
    std::shared_ptr<std::atomic<bool>> preparing = std::make_shared<std::atomic<bool>>(false);
    dev_t device = 0;
    ino_t inode = 0;
    // Control-thread-owned bounded cache makes a retried request idempotent.
    std::array<std::array<char, 33>, 32> acceptedNonces {};
    size_t nextNonce = 0;
    Impl(Engine& owner, const char* socketPath, bool own = false) : engine(owner), path(socketPath), ownedRuntime(own) {
        if (ownedRuntime) runtime=juce::File(path).getParentDirectory().getFullPathName().toStdString();
        cpu_set_t allowed;
        CPU_ZERO(&allowed);
        if (::sched_getaffinity(0,sizeof(allowed),&allowed)!=0)
            throw std::runtime_error("Cannot discover safety CPUs");
        const auto& config=engine.getRealtimeConfig();
        for(int cpu=0;cpu<CPU_SETSIZE;++cpu) {
            if(!CPU_ISSET(cpu,&allowed)) continue;
            if(std::find(config.audioCpus.begin(),config.audioCpus.end(),cpu)!=config.audioCpus.end()) continue;
            if(std::find(config.workerCpus.begin(),config.workerCpus.end(),cpu)!=config.workerCpus.end()) continue;
            controlCpus.push_back(cpu);
        }
        if(controlCpus.empty()) for(int cpu=0;cpu<CPU_SETSIZE;++cpu)
            if(CPU_ISSET(cpu,&allowed)) controlCpus.push_back(cpu);
        if(const auto* cpus=std::getenv("AEOLUS_CONTROL_CPUS")) {
            const auto selected=RealtimeConfig::parseCpuList(cpus);
            if(selected.empty()) throw std::runtime_error("Invalid control CPU list");
            for(const auto cpu:selected) if(cpu>=CPU_SETSIZE || !CPU_ISSET(cpu,&allowed))
                throw std::runtime_error("Control CPU is unavailable");
            controlCpus=selected;
        }
        if(const auto* priority=std::getenv("AEOLUS_CONTROL_RT_PRIORITY")) {
            char* end=nullptr;const auto value=std::strtol(priority,&end,10);
            if(end==priority || *end || value<0 || value>70) throw std::runtime_error("Invalid control priority");
            controlPriority=int(value);
        }
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
    void startWatchdog() {
        if(!ownedRuntime) return;
        const auto executable=juce::File::getSpecialLocation(juce::File::currentExecutableFile);
        const auto helper=executable.getSiblingFile("helpers").getChildFile("aeolus_safety.py");
        if(!helper.existsAsFile()) return;
        juce::StringArray names;
        for(const auto cpu:controlCpus) names.add(juce::String(cpu));
        std::vector<std::string> arguments { AEOLUS_SAFETY_PYTHON, helper.getFullPathName().toStdString(),
            "--pid",std::to_string(::getpid()),"--executable",executable.getFullPathName().toStdString(),
            "--runtime",runtime,"--cpus",names.joinIntoString(",").toStdString(),
            "--reports",getOrganDataDirectory().getChildFile("SafetyReports").getFullPathName().toStdString() };
        std::vector<char*> argv;
        for(auto& value:arguments) argv.push_back(value.data());
        argv.push_back(nullptr);
        posix_spawn_file_actions_t actions;
        posix_spawn_file_actions_init(&actions);
        posix_spawn_file_actions_addopen(&actions,0,"/dev/null",O_RDONLY,0);
        posix_spawn_file_actions_addopen(&actions,1,"/dev/null",O_WRONLY,0);
        posix_spawn_file_actions_addopen(&actions,2,(runtime+"/helper.log").c_str(),O_WRONLY|O_CREAT|O_EXCL,0600);
        posix_spawnattr_t attributes;
        posix_spawnattr_init(&attributes);
#ifdef POSIX_SPAWN_SETSID
        posix_spawnattr_setflags(&attributes,POSIX_SPAWN_SETSID);
#endif
        const int result=posix_spawnp(&watcher,argv.front(),&actions,&attributes,argv.data(),environ);
        posix_spawnattr_destroy(&attributes);
        posix_spawn_file_actions_destroy(&actions);
        if(result!=0) watcher=-1;
    }
    bool watchdogRunning() const {
        if(!ownedRuntime || watcher<=0) return false;
        siginfo_t status {};
        return ::waitid(P_PID,id_t(watcher),&status,WEXITED|WNOHANG|WNOWAIT)==0 && status.si_pid==0;
    }
    bool watchdogReady() const {
        return watchdogRunning() && juce::File(runtime).getChildFile("ready").existsAsFile();
    }
    void beginShutdown() {
        if(ownedRuntime) {
            const int marker=::open((runtime+"/closing").c_str(),O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC,0600);
            if(marker>=0) ::close(marker);
        }
        alive->store(false,std::memory_order_release);
        stopping.store(true,std::memory_order_release);
        if(thread.joinable()) thread.join();
    }
    ~Impl() {
        beginShutdown();
        if(fd>=0) ::close(fd);
        struct stat info {};
        if(::lstat(path.c_str(),&info)==0 && info.st_dev==device && info.st_ino==inode)
            ::unlink(path.c_str());
        // Engine teardown completed. Its process may immediately construct a
        // replacement Engine; the old companion must detach, never kill that PID.
        if(ownedRuntime) {
            const int marker=::open((runtime+"/detached").c_str(),O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC,0600);
            if(marker>=0) ::close(marker);
            const bool retired=watcher<=0 || ::waitpid(watcher,nullptr,WNOHANG)==watcher;
            if(retired) {
                // Retain nonempty startup diagnostics, otherwise reclaim an
                // unsupervised runtime even when Python/helpers were missing.
                if(juce::File(runtime).getChildFile("helper.log").getSize()==0) {
                    for(const auto* name:{"ready","closing","detached","helper.log","watchdog.jsonl","panel.jsonl"})
                        ::unlink((runtime+"/"+name).c_str());
                    ::rmdir(runtime.c_str());
                }
            }
        }
    }
    void resumeOnMessageThread() {
        // Only explicit Resume restarts audio. Device stop/reprepare happens
        // outside the callback and clears old reverb/filter history safely.
        std::weak_ptr<std::atomic<bool>> weak = alive;
        if (preparing->exchange(true, std::memory_order_acq_rel)) return;
        const auto transition = preparing;
        const auto epoch=engine.getPanicRequested();
        Engine* owner=&engine;
        const bool posted = juce::MessageManager::callAsync([weak, transition, epoch, owner] {
            const auto guard = weak.lock();
            if (!guard || !guard->load(std::memory_order_acquire)) {
                transition->store(false, std::memory_order_release);
                return;
            }
            if(!owner->authorizeEmergencyResume(epoch)) {
                transition->store(false,std::memory_order_release);
                return;
            }
#if JucePlugin_Build_Standalone
            if (auto* holder = juce::StandalonePluginHolder::getInstance()) {
                holder->deviceManager.closeAudioDevice();
                holder->deviceManager.restartLastAudioDevice();
            }
#endif
            owner->cancelEmergencyResume();
            transition->store(false, std::memory_order_release);
        });
        if (!posted) preparing->store(false, std::memory_order_release);
    }
    void run() noexcept {
        try {
            rt::setCurrentThreadName("aeolus-panic");
            rt::applyToCurrentThread(controlPriority, controlCpus);
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
#if AEOLUS_TESTING && JucePlugin_Build_Standalone
                if(std::strcmp(command,"RESET_TEST")==0) {
                    std::weak_ptr<std::atomic<bool>> weak=alive;
                    juce::MessageManager::callAsync([weak] {
                        const auto live=weak.lock();
                        if(live && live->load(std::memory_order_acquire))
                            if(auto* holder=juce::StandalonePluginHolder::getInstance()) {
                                for(int i=0;i<juce::TopLevelWindow::getNumTopLevelWindows();++i) {
                                    auto* window=dynamic_cast<juce::StandaloneFilterWindow*>(juce::TopLevelWindow::getTopLevelWindow(i));
                                    if(window && &window->getDeviceManager()==&holder->deviceManager) {
                                        window->resetToDefaultState();
                                        return;
                                    }
                                }
                            }
                    });
                    continue;
                }
#endif
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
    bool watchdogReady() const { return false; }
    bool watchdogRunning() const { return false; }
    void beginShutdown() {}
#endif
};

PanicControl::PanicControl(std::unique_ptr<Impl> implementation) : impl(std::move(implementation)) {}
PanicControl::~PanicControl() = default;
bool PanicControl::watchdogReady() const { return impl->watchdogReady(); }
bool PanicControl::watchdogRunning() const { return impl->watchdogRunning(); }
void PanicControl::beginShutdown() { impl->beginShutdown(); }
std::unique_ptr<PanicControl> PanicControl::create(Engine& engine) {
#if JUCE_LINUX
    if (const char* path = std::getenv("AEOLUS_PANIC_SOCKET")) {
        if (*path) return std::unique_ptr<PanicControl>(new PanicControl(std::make_unique<Impl>(engine, path)));
    }
#if JucePlugin_Build_Standalone
#if AEOLUS_TESTING
    const bool automatic=juce::SystemStats::getEnvironmentVariable("AEOLUS_AUTO_SAFETY",{})=="1";
#else
    const bool automatic=juce::SystemStats::getEnvironmentVariable("AEOLUS_AUTO_SAFETY","1")!="0";
#endif
    if(automatic) {
        std::string runtime;
        try {
            std::string pattern="/dev/shm/aeolus-safety-XXXXXX";
            char* directory=::mkdtemp(pattern.data());
            if(directory==nullptr) throw std::runtime_error("Cannot create private safety runtime");
            runtime=directory;
            auto state=std::make_unique<Impl>(engine,(runtime+"/control.sock").c_str(),true);
            state->startWatchdog();
            return std::unique_ptr<PanicControl>(new PanicControl(std::move(state)));
        } catch(const std::exception& error) {
            if(!runtime.empty()) ::rmdir(runtime.c_str());
            juce::Logger::writeToLog("Aeolus independent safety unavailable: "+juce::String(error.what()));
            return {};
        }
    }
#endif
#else
    juce::ignoreUnused(engine);
#endif
    return {};
}
AEOLUS_NAMESPACE_END
