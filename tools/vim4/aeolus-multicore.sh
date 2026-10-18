#!/bin/sh
# Launch Aeolus with real-time scheduling and the multicore batch renderer.
# Defaults suit the Khadas VIM4 (CPUs 0-3 = Cortex-A73 performance cluster).
# Reports the system state but never changes governors, limits or thermals.
# See docs/MULTICORE.md.

here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
: "${AEOLUS_BIN:=$here/../../build/Aeolus_artefacts/Release/Standalone/Aeolus}"

: "${AEOLUS_AUDIO_RT_PRIORITY:=70}"
: "${AEOLUS_AUDIO_CPUS:=0}"
: "${AEOLUS_DSP_WORKERS:=3}"
: "${AEOLUS_DSP_WORKER_CPUS:=1-3}"
: "${AEOLUS_MAX_VOICES:=1024}"
export AEOLUS_AUDIO_RT_PRIORITY AEOLUS_AUDIO_CPUS AEOLUS_DSP_WORKERS AEOLUS_DSP_WORKER_CPUS AEOLUS_MAX_VOICES

if [ ! -x "$AEOLUS_BIN" ]; then
    echo "Aeolus executable not found: $AEOLUS_BIN (set AEOLUS_BIN)" >&2
    exit 1
fi

echo "Aeolus multicore: audio CPU $AEOLUS_AUDIO_CPUS, $AEOLUS_DSP_WORKERS workers on CPUs $AEOLUS_DSP_WORKER_CPUS," \
     "SCHED_FIFO $AEOLUS_AUDIO_RT_PRIORITY, voice pool $AEOLUS_MAX_VOICES" >&2

rtlimit=$(ulimit -r 2>/dev/null || echo 0)
if [ "$rtlimit" != "unlimited" ] && [ "${rtlimit:-0}" -lt "$AEOLUS_AUDIO_RT_PRIORITY" ] 2>/dev/null; then
    echo "warning: real-time priority limit is $rtlimit (< $AEOLUS_AUDIO_RT_PRIORITY); SCHED_FIFO will be refused." >&2
fi

for cpu in /sys/devices/system/cpu/cpu[0-9]*; do
    [ -r "$cpu/cpu_capacity" ] && printf '%s capacity %s\n' "${cpu##*/}" "$(cat "$cpu/cpu_capacity")" >&2
done

for policy in /sys/devices/system/cpu/cpufreq/policy*; do
    [ -r "$policy/scaling_governor" ] || continue
    printf '%s: governor %s, %s kHz (max %s)\n' "${policy##*/}" "$(cat "$policy/scaling_governor")" \
        "$(cat "$policy/scaling_cur_freq" 2>/dev/null)" "$(cat "$policy/scaling_max_freq" 2>/dev/null)" >&2
done

for zone in /sys/class/thermal/thermal_zone*; do
    [ -r "$zone/temp" ] && printf '%s (%s): %s m°C\n' "${zone##*/}" "$(cat "$zone/type" 2>/dev/null)" "$(cat "$zone/temp")" >&2
done

exec "$AEOLUS_BIN" "$@"
