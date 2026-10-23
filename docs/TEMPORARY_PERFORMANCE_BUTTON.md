# Temporary Linux CPU performance button

The console footer includes a **Performance mode** button. It discovers Linux
CPU-frequency policies and shows **Performance ON** when every detected policy
reports the performance governor. The status refreshes once per second. This is
a temporary system control, separate from the planned portable hardware profiles
and protected installer; it does not implement those roadmap features.

When needed, clicking requests desktop administrator authentication through
`pkexec --disable-internal-agent`, invoking an installed system `cpupower` binary
with fixed arguments `-c all frequency-set -g performance`. Aeolus stays
unprivileged; no password is collected/stored by Aeolus, no shell/helper script
is elevated, and no passwordless authorization or startup rule is installed.
Ubuntu's versioned `/usr/lib/linux-tools/*/cpupower` binary is preferred over its
kernel-specific `/usr/bin/cpupower` wrapper, which may not work on vendor kernels.
Installed version directories are examined in descending natural version order.
Otherwise `/usr/bin/cpupower` is used.

This affects the whole machine and may increase power consumption and heat.
It does not change DSP workers, voice capacity, CPU affinity, audio priority or
sound processing. It is not persistent: reboot or power-management services may
change the governor again. Restore the previous governor through the operating
system's CPU/power controls; this temporary button is an enable action, not a
save/restore profile or an uninstall tool.

Unsupported policies, missing tools and non-Linux systems disable the button
with an explanatory tooltip. Linux also needs a working desktop authentication
agent and glibc 2.34 or newer (the temporary control closes inherited file
descriptors atomically with `posix_spawn_file_actions_addclosefrom_np`; older libc
versions leave this control disabled). Complete or cancel its dialog normally; audio and Panic remain available.
Only one request may be pending per Aeolus process, including across editor
closure/reopening. Closing the editor does not cancel an already authorized system
operation. A process-wide message-thread monitor collects child exit status even without an
editor. Linux `posix_spawn` avoids JUCE's `fork()` launch path. No captured
output, blocking wait or editor callback is used. Application shutdown requests
authentication cancellation; an already authorized privileged action may finish. When the
operation ends, Aeolus checks its exit status and rereads every policy; failure
or cancellation produces an explanation. A failed command can have changed some
policies; there is no automatic rollback. No clean-audio guarantee follows from
an ON indicator.

Validation records are in the implementation report. Actual administrator
success/cancellation and target-board listening require the real desktop and user.

## Local validation — October 21, 2026

- Linux x86-64 Release build and CTest: 2/2 passed, 17.39 seconds. The simulated
  policy fixture covers active/inactive policies, a non-performance governor and
  an unreadable active governor. Footer checks cover 640x480 with the keyboard
  shown/hidden; no test invokes pkexec or changes a real governor.
- Production x86-64 standalone built with tests/profile mode disabled; dynamic
  symbol/dependency check (`ldd -r`) passed.
- Claude review `20261021T231554Z-1e25e0ab1b`: completed on the current UI/test
  snapshot with zero findings. Static review only; Codex performed the checks.
- Authentication, cancellation, application shutdown during authentication and
  physical listening remain manual checks. No Windows/macOS build or general
  hardware-support/performance claim follows from these local results.

## VIM4 live check — October 21, 2026

A separate ARM64 production standalone was built with Release/IPO enabled,
tests/profile mode disabled and stereo output. Its selected source hashes match
the local UI, processor, engine and CMake files. Native `ldd -r` passed and all
four safety companions are adjacent to the executable.

The new executable is running unprivileged with the existing explicit VIM4 test
profile: three DSP workers, a 1024-entry pool, audio CPU 0 and worker CPUs 1–3,
without changing scheduling priority. FiiO playback was confirmed running in
stereo at 44.1 kHz; the callback log reports 512 frames. Read-only HEALTH/STATUS
queries confirmed active audio and an unmuted engine. These are startup checks,
not a sustainable-voice or zero-xrun benchmark.

The user confirmed the footer button is visible and the organ plays normally.
Both online CPU policies were already `performance`, so no administrator action
was invoked. Authentication success/cancellation and quitting while authenticating
remain unverified. After a reboot or policy change, the button should offer the
administrator action again; the ON indicator always reflects sampled policy state.

A fresh backup of organ/settings files was made before launch. The organ
configuration and 85 stop-model hashes were unchanged after startup; ordinary
application state/preferences may still save normally. The prior executable is
preserved. The new opt-in desktop launcher is
`Start Aeolus Performance Test-2026-10-21.sh`; it uses the separate
`build/performance-button/` production build and retains environment overrides.
This temporary launcher is specific to the existing VIM4 test arrangement; the
button's Linux policy discovery has no fixed core numbers or board identity.

### Post-reboot apply check

After reboot, both policies were observed at `conservative`. The user then
reported the performance button changed state; a fresh remote check confirmed
both `policy0` and `policy4` at `performance`, with the new executable running.
The FiiO had renumbered to ALSA card 1 and Aeolus opened `/dev/snd/pcmC1D0p`.
This confirms the requested live governor change and DAC selection after reboot.
The authentication dialog itself, cancellation and shutdown during authentication
were not separately observed; those interaction checks remain open.
