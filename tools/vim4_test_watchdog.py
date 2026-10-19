#!/usr/bin/env python3
"""Supervise one isolated Aeolus test process; never discard DSP jobs in flight.

Legacy CSV observation does not distinguish logger/UI blockage from audio stalls.
The protected launcher requires independent HEALTH v2 callback counters instead;
CSV/UI staleness is diagnostic only. Default generic recovery removes FIFO/RR
priority; process termination requires explicit generic or panic authorization.
"""
import argparse
import csv
import contextlib
import json
import os
from pathlib import Path
import signal
import time


def identity(pid):
    proc = Path('/proc', str(pid))
    fields = (proc / 'stat').read_text().rpartition(') ')[2].split()
    return proc.stat().st_uid, fields[19], fields[0], Path(os.readlink(proc / 'exe'))


def snapshot(pid):
    threads = []
    for task in Path('/proc', str(pid), 'task').glob('*'):
        try:
            tid = int(task.name)
            threads.append({'tid': tid, 'name': (task / 'comm').read_text().strip(),
                            'cpus': sorted(os.sched_getaffinity(tid)),
                            'policy': os.sched_getscheduler(tid),
                            'priority': os.sched_getparam(tid).sched_priority,
                            'wait_channel': (task / 'wchan').read_text().strip()})
        except (FileNotFoundError, ProcessLookupError):
            pass
    return threads


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--pid', type=int, required=True)
    parser.add_argument('--executable', type=Path, required=True)
    parser.add_argument('--timing-file', type=Path, required=True)
    parser.add_argument('--log', type=Path, required=True)
    parser.add_argument('--cpus', default='4,5,6,7')
    parser.add_argument('--ready-file', type=Path)
    parser.add_argument('--shutdown-file', type=Path)
    parser.add_argument('--detach-file', type=Path)
    parser.add_argument('--shutdown-timeout', type=float, default=30)
    parser.add_argument('--stalled-after', type=float, default=5)
    parser.add_argument('--startup-timeout', type=float, default=90)
    parser.add_argument('--wait-for-audio', action='store_true')
    parser.add_argument('--resume-timeout', type=float, default=30)
    parser.add_argument('--poll', type=float, default=.25)
    parser.add_argument('--monitor-only', action='store_true')
    parser.add_argument('--rt-priority', type=int, default=0)
    parser.add_argument('--panic-socket', type=Path)
    parser.add_argument('--require-audio-heartbeat', action='store_true',
                        help='Use HEALTH v2 callback completion counters; never recover from stale CSV alone')
    parser.add_argument('--allow-stop', action='store_true',
                        help='Allow the panic protocol to stop only an unacknowledging target')
    parser.add_argument('--panic-timeout', type=float, default=1)
    parser.add_argument('--terminate-after', type=float,
                        help='Explicitly allow SIGTERM after this many seconds without progress')
    parser.add_argument('--kill-after', type=float,
                        help='Explicitly allow SIGKILL this many seconds after SIGTERM')
    args = parser.parse_args()
    if args.pid <= 1 or args.pid == os.getpid():
        parser.error('Specify the isolated test application PID')
    if min(args.poll, args.stalled_after, args.startup_timeout, args.resume_timeout, args.shutdown_timeout) <= 0:
        parser.error('Timeouts and polling interval must be positive')
    if args.terminate_after is not None and args.terminate_after <= args.stalled_after:
        parser.error('--terminate-after must exceed --stalled-after')
    if args.kill_after is not None and (args.terminate_after is None or args.kill_after <= 0):
        parser.error('--kill-after requires --terminate-after and a positive delay')
    if args.panic_socket is not None and (args.terminate_after is not None or args.monitor_only):
        parser.error('Panic protocol cannot be combined with generic termination or monitor-only mode')
    if args.allow_stop and args.panic_socket is None:
        parser.error('--allow-stop requires --panic-socket')
    if args.require_audio_heartbeat and args.panic_socket is None:
        parser.error('--require-audio-heartbeat requires --panic-socket')
    if args.panic_socket is not None:
        # Panic supervision must never silently fall back to CSV-based recovery.
        args.require_audio_heartbeat = True
    if args.panic_timeout <= 0 or not 0 <= args.rt_priority <= 70:
        parser.error('Invalid panic timeout or real-time priority')
    cpus = {int(cpu) for cpu in args.cpus.split(',')}
    os.sched_setaffinity(0, cpus)
    if args.rt_priority:
        os.sched_setscheduler(0, os.SCHED_FIFO, os.sched_param(args.rt_priority))
    original = identity(args.pid)
    expected = args.executable.resolve(strict=True)
    if original[0] != os.getuid() or original[2] == 'Z' or original[3] != expected:
        parser.error('Target must be a live process owned by this user with the exact specified executable')

    def same_process():
        try:
            current = identity(args.pid)
            return current[0:2] == original[0:2] and current[2] != 'Z' and current[3] == expected
        except (FileNotFoundError, ProcessLookupError):
            return False

    # Exclusive log creation prevents overwriting evidence or user data.
    with contextlib.ExitStack() as scope:
        pidfd = os.pidfd_open(args.pid)
        scope.callback(os.close, pidfd)
        if not same_process():
            return 0
        log = scope.enter_context(args.log.open('x'))
        def emit(event, **data):
            row = {'event': event, 'time_ns': time.monotonic_ns(), 'pid': args.pid, **data}
            log.write(json.dumps(row) + '\n')
            log.flush()
            print(json.dumps(row), flush=True)

        emit('started', start_ticks=original[1], executable=str(expected), cpus=sorted(cpus),
             monitor_only=args.monitor_only, terminate_after=args.terminate_after,
             scheduler=os.sched_getscheduler(0), priority=os.sched_getparam(0).sched_priority,
             heartbeat_source=('independent HEALTH v2 callback counters' if args.require_audio_heartbeat
                               else 'callback CSV (not an independent audio heartbeat)'),
             csv_monitoring=not args.require_audio_heartbeat)
        closing_started = None
        closing_recovery = False
        manual_pending_started = None
        recovery = None
        if args.panic_socket is not None:
            from aeolus_panic_client import PanicRecovery
            recovery = PanicRecovery(args.pid, expected, args.panic_socket, emit,
                                     timeout=args.panic_timeout, allow_stop=args.allow_stop)
            scope.callback(recovery.close)
        start = time.monotonic()
        last_progress = None
        offset = 0
        pending = ''
        header = None
        armed = False
        stalled = False
        demoted = False
        term_sent = None
        resume_started = None
        audio_progress = gui_progress = None
        last_audio_progress = last_gui_progress = start
        ui_stalled = inactive = False
        while same_process():
            if args.detach_file is not None and args.detach_file.exists():
                emit('engine_detached')
                return 0
            if args.shutdown_file is not None and args.shutdown_file.exists():
                if closing_started is None:
                    closing_started=time.monotonic()
                    emit('application_closing')
                if time.monotonic()-closing_started<args.shutdown_timeout:
                    time.sleep(args.poll)
                    continue
                if recovery is not None and not closing_recovery:
                    closing_recovery=True
                    emit('application_close_stalled')
                    recovery.request()
            if recovery is not None and not recovery.tick():
                break
            if recovery is not None:
                if (recovery.client.requested>recovery.client.acknowledged
                        and recovery.client.audio_state==1 and not recovery.client.resuming):
                    if manual_pending_started is None:manual_pending_started=time.monotonic()
                    if time.monotonic()-manual_pending_started>=args.panic_timeout and recovery.state not in ('pending','terminating','killed'):
                        emit('manual_panic_unacknowledged')
                        recovery.request()
                else:manual_pending_started=None
            if args.require_audio_heartbeat:
                recovery.client.send('HEALTH')
            if not args.require_audio_heartbeat:
                # Read a bounded tail, handling partially written rows and truncation.
                try:
                    with args.timing_file.open() as stream:
                        size = os.fstat(stream.fileno()).st_size
                        if size < offset:
                            offset, pending, header = 0, '', None
                        if header is None:
                            header_line = stream.readline()
                            if not header_line.endswith('\n'):
                                raise StopIteration
                            header = next(csv.reader([header_line]))
                            offset = stream.tell()
                        if size - offset > 262144:
                            stream.seek(size - 262144)
                            stream.readline()
                            pending = ''
                        else:
                            stream.seek(offset)
                        data = pending + stream.read(262144)
                        offset = stream.tell()
                    lines = data.split('\n')
                    pending = lines.pop()
                    for line in lines:
                        if not line or line.startswith('#'):
                            continue
                        try:
                            row = dict(zip(header, next(csv.reader([line]))))
                            completed = (int(row['start_ns']) + int(row['duration_ns'])) / 1e9
                            now = time.monotonic()
                            if completed <= now and (last_progress is None or completed > last_progress):
                                last_progress = completed
                        except (ValueError, KeyError):
                            continue
                except (FileNotFoundError, StopIteration):
                    pass
            now = time.monotonic()
            heartbeat = recovery.client if args.require_audio_heartbeat else None
            if heartbeat is not None:
                if heartbeat.audio_completed is not None and heartbeat.audio_completed != audio_progress:
                    audio_progress = heartbeat.audio_completed
                    last_audio_progress = now
                if heartbeat.gui_progress is not None and heartbeat.gui_progress != gui_progress:
                    gui_progress = heartbeat.gui_progress
                    last_gui_progress = now
                if armed and now-last_gui_progress >= args.stalled_after and not ui_stalled:
                    ui_stalled = True
                    emit('ui_progress_stalled', audio_completed=audio_progress)
                elif ui_stalled and now-last_gui_progress < args.stalled_after:
                    ui_stalled = False
                    emit('ui_progress_resumed')
            progress_observed = (audio_progress is not None and audio_progress > 0) if heartbeat is not None else last_progress is not None
            if progress_observed and not armed:
                armed = True
                emit('armed')
                if args.ready_file is not None:
                    with args.ready_file.open('x') as ready:
                        ready.write(str(args.pid)+'\n')
            if not args.wait_for_audio and not armed and now - start >= args.startup_timeout:
                emit('startup_without_telemetry', threads=snapshot(args.pid))
                return 2  # No blind recovery before callbacks have been observed.
            age = (now-last_audio_progress if heartbeat is not None
                   else now-last_progress if last_progress is not None else 0)
            if heartbeat is not None and heartbeat.health_received_at is not None:
                age = max(age, now-heartbeat.health_received_at)
            # Explicit Resume stops/reprepares the device on the UI thread.
            # Allow a bounded setup interval while the control endpoint is alive.
            reprepare_expired = False
            preparing = not closing_recovery and recovery is not None and (recovery.client.resuming or
                        (heartbeat is not None and heartbeat.audio_state == 2))
            if preparing:
                if resume_started is None:
                    resume_started = now
                    emit('audio_reprepare_started')
                if now - resume_started < args.resume_timeout:
                    time.sleep(args.poll)
                    continue
                reprepare_expired = True
            else:
                if resume_started is not None:
                    # A successfully prepared device needs a fresh callback
                    # window even if its first completed callback is not visible.
                    last_audio_progress = now
                    age = 0
                    emit('audio_reprepare_finished')
                resume_started = None
            if heartbeat is not None:
                # A deliberately closed device may have no callbacks. Only a
                # fresh response can grant this exemption; a frozen endpoint
                # still times out and follows the normal recovery sequence.
                fresh = heartbeat.health_received_at is not None and now-heartbeat.health_received_at < args.stalled_after
                if (fresh and heartbeat.audio_state == 0 and not reprepare_expired and not closing_recovery
                        and heartbeat.audio_started == heartbeat.audio_completed):
                    if not inactive:
                        inactive = True
                        emit('audio_inactive')
                    last_audio_progress = now
                    time.sleep(args.poll)
                    continue
                if inactive and fresh and heartbeat.audio_active and not reprepare_expired:
                    inactive = False
                    last_audio_progress = now
                    age = 0
                    emit('audio_active')
            if armed and age >= args.stalled_after:
                if not stalled:
                    stalled = True
                    emit('audio_progress_stalled' if heartbeat is not None else 'telemetry_stalled',
                         stale_seconds=age, threads=snapshot(args.pid))
                    if recovery is not None:
                        recovery.request()
                if recovery is None and not args.monitor_only and not demoted:
                    changes = []
                    for thread in snapshot(args.pid):
                        if not same_process():
                            break
                        if thread['policy'] in (os.SCHED_FIFO, os.SCHED_RR):
                            try:
                                # A vanished/reused TID must still belong to this process.
                                if not Path('/proc', str(args.pid), 'task', str(thread['tid'])).exists():
                                    continue
                                os.sched_setscheduler(thread['tid'], os.SCHED_OTHER, os.sched_param(0))
                                changes.append({'tid': thread['tid'], 'demoted': True})
                            except (PermissionError, ProcessLookupError) as error:
                                changes.append({'tid': thread['tid'], 'error': str(error)})
                    demoted = True
                    emit('realtime_priority_removed', changes=changes)
                if args.terminate_after is not None and age >= args.terminate_after and term_sent is None:
                    if same_process():
                        signal.pidfd_send_signal(pidfd, signal.SIGTERM)
                        term_sent = now
                        emit('sigterm_sent')
            elif stalled:
                stalled = False
                emit('progress_resumed', realtime_priority_remains_removed=demoted)
            # An explicitly requested hard-stop escalation survives renewed telemetry.
            if term_sent is not None and args.kill_after is not None and now - term_sent >= args.kill_after:
                if same_process():
                    signal.pidfd_send_signal(pidfd, signal.SIGKILL)
                    emit('sigkill_sent')
                return 3
            time.sleep(args.poll)
        emit('target_exited')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
