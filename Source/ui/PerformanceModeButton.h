#pragma once

#include <JuceHeader.h>
#if JUCE_LINUX
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>
#include <cerrno>
#include <csignal>
extern char** environ;
#endif

namespace ui {

// Temporary, explicit Linux governor control. No changes to audio scheduling,
// affinity, application defaults or persistent system policy.
class PerformanceModeButton final : public juce::TextButton, private juce::Timer
{
public:
    PerformanceModeButton() : juce::TextButton("Performance mode")
    {
        setName("temporaryPerformanceMode");
        onClick = [this] { request(); };
        _seenCompletion = operation().completion;
        refresh();
        startTimer(1000);
    }

    ~PerformanceModeButton() override { stopTimer(); }

    static juce::Array<juce::File> policies(const juce::File& root)
    {
        auto paths = root.findChildFiles(juce::File::findDirectories, false, "policy*");
        for (int i = paths.size(); --i >= 0;)
            if (paths[i].getChildFile("affected_cpus").loadFileAsString().trim().isEmpty())
                paths.remove(i); // Inactive policies have no online CPUs.
        return paths;
    }

    static bool allPerformance(const juce::Array<juce::File>& paths)
    {
        if (paths.isEmpty()) return false;
        for (const auto& path : paths)
            if (path.getChildFile("scaling_governor").loadFileAsString().trim() != "performance")
                return false;
        return true;
    }

private:
    // Independent message-thread owner reaps the child even with no editor.
    // JUCE shutdown destroys it before the message subsystem disappears.
    struct Operation : private juce::Timer, public juce::DeletedAtShutdown {
        bool pending = false;
        bool failed = false;
        unsigned completion = 0;
        unsigned reportedCompletion = 0;
       #if JUCE_LINUX
        pid_t pid = -1;
       #endif
        ~Operation() override {
            stopTimer();
           #if JUCE_LINUX
            if (pending && pid > 0) {
                // Cancel authentication on application shutdown. An already
                // authorized privileged operation may have begun and can finish.
                ::kill(pid, SIGTERM);
                int status = 0;
                ::waitpid(pid, &status, WNOHANG);
            }
           #endif
            operationSlot() = nullptr;
        }
       #if JUCE_LINUX
        bool start(const juce::StringArray& args) {
            posix_spawn_file_actions_t actions;
            if (posix_spawn_file_actions_init(&actions) != 0) return false;
            int error = ENOTSUP;
           #if defined(__GLIBC_PREREQ)
            #if __GLIBC_PREREQ(2, 34)
            // Do not let authentication retain audio, MIDI or safety descriptors.
            error = posix_spawn_file_actions_addclosefrom_np(&actions, 3);
            #endif
           #endif
            for (int fd = 0; fd < 3 && error == 0; ++fd)
                error = posix_spawn_file_actions_addopen(&actions, fd, "/dev/null",
                                                       fd == 0 ? O_RDONLY : O_WRONLY, 0);
            std::vector<char*> argv;
            for (const auto& arg : args)
                argv.push_back(const_cast<char*>(arg.toRawUTF8()));
            argv.push_back(nullptr);
            if (error == 0)
                error = ::posix_spawn(&pid, "/usr/bin/pkexec", &actions, nullptr,
                                      argv.data(), environ);
            posix_spawn_file_actions_destroy(&actions);
            pending = error == 0;
            if (pending) startTimer(200);
            return pending;
        }
        void timerCallback() override {
            int status = 0;
            const auto result = ::waitpid(pid, &status, WNOHANG);
            if (result == 0 || (result < 0 && errno == EINTR)) return;
            failed = result < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0;
            pending = false;
            pid = -1;
            ++completion;
            stopTimer();
        }
       #else
        void timerCallback() override {}
       #endif
    };
    static Operation*& operationSlot() { static Operation* value = nullptr; return value; }
    static Operation& operation() {
        auto*& value = operationSlot();
        if (value == nullptr) value = new Operation;
        return *value;
    }
    unsigned _seenCompletion = 0;

    static juce::File tool()
    {
       #if JUCE_LINUX
        // Ubuntu's /usr/bin/cpupower wrapper may require a missing exact-kernel
        // tools package. The installed OS-owned binary uses the cpufreq ABI.
        auto directories = juce::File("/usr/lib/linux-tools").findChildFiles(
            juce::File::findDirectories, false);
        std::sort(directories.begin(), directories.end(), [](const auto& a, const auto& b) {
            return a.getFileName().compareNatural(b.getFileName()) > 0;
        });
        for (const auto& directory : directories)
            if (directory.getChildFile("cpupower").existsAsFile())
                return directory.getChildFile("cpupower");
        return juce::File("/usr/bin/cpupower");
       #else
        return {};
       #endif
    }

    static const juce::File& installedTool() {
        static const auto value = tool();
        return value;
    }

    static bool canSpawnSafely() {
       #if JUCE_LINUX && defined(__GLIBC_PREREQ)
        #if __GLIBC_PREREQ(2, 34)
        return true;
        #endif
       #endif
        return false;
    }

    void request()
    {
       #if JUCE_LINUX
        auto& state = operation();
        if (state.pending || !canSpawnSafely()) return;
        const auto executable = installedTool();
        // Argument array, fixed operation, existing system executable: no shell,
        // mutable application helper, stored password or new authorization rule.
        const juce::StringArray args { "/usr/bin/pkexec", "--disable-internal-agent",
            executable.getFullPathName(), "-c", "all", "frequency-set", "-g", "performance" };
        state.start(args); // posix_spawn avoids fork-induced audio-buffer COW faults.
        if (!state.pending)
            report("Could not start administrator authentication. Check that pkexec and cpupower are installed.");
        refresh();
       #endif
    }

    void timerCallback() override
    {
        auto& state = operation();
        if (_seenCompletion != state.completion) {
            _seenCompletion = state.completion;
            const bool unreported = state.reportedCompletion != state.completion;
            state.reportedCompletion = state.completion;
            if (unreported && (state.failed || !allPerformance(policies(juce::File("/sys/devices/system/cpu/cpufreq")))))
                report("Performance mode was not verified on every CPU policy. Authentication may have been cancelled, an authentication agent may be unavailable, or the system may have refused the change. Some policies may have changed; check the system CPU settings before retrying.");
        }
        refresh();
    }

    void refresh()
    {
       #if JUCE_LINUX
        const auto paths = policies(juce::File("/sys/devices/system/cpu/cpufreq"));
        const bool active = allPerformance(paths);
        const bool pending = operation().pending;
        setButtonText(pending ? "Authenticating..." : active ? "Performance ON" : "Performance mode");
        bool supported = !paths.isEmpty();
        for (const auto& path : paths)
            supported = supported && juce::StringArray::fromTokens(
                path.getChildFile("scaling_available_governors").loadFileAsString(), " \t\r\n", "").contains("performance");
        const bool tools = juce::File("/usr/bin/pkexec").existsAsFile() && installedTool().existsAsFile();
        setEnabled(!pending && !active && supported && tools && canSpawnSafely());
        setTooltip(pending ? "Complete or cancel the system authentication dialog. Panic remains available."
            : active ? "All detected CPU policies use performance. Temporary system-wide setting; may reset at reboot or be changed by power management."
            : !supported ? "CPU performance governor is unavailable on this system."
            : !canSpawnSafely() ? "Temporary control requires Linux glibc 2.34 or newer to close inherited audio/device descriptors safely."
            : !tools ? "Requires installed system pkexec and cpupower tools, plus a desktop authentication agent."
            : "Temporarily set all CPU policies to performance using administrator authentication. Affects the whole system; may increase power use and heat. Resets according to system policy, usually at reboot.");
       #else
        setEnabled(false);
        setTooltip("Temporary CPU performance control is available on Linux only.");
       #endif
    }

    static void report(const juce::String& text)
    {
        juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon,
                                              "CPU performance mode", text);
    }
};
}
