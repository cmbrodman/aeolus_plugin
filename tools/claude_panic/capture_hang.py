#!/usr/bin/env python3
"""Observe an owned isolated Aeolus process; capture stacks before watchdog recovery.

This observer never changes scheduling or terminates Aeolus. GDB briefly stops
the target; timeout cleanup resumes only this owned target. Captured periods
are unsuitable for performance comparisons. Put output on RAM-backed storage.
"""
import argparse
import json
import os
from pathlib import Path
import platform
import select
import signal
import subprocess
import time


def identity(pid):
    proc = Path('/proc', str(pid))
    fields = (proc / 'stat').read_text().rpartition(') ')[2].split()
    return proc.stat().st_uid, fields[19], (proc / 'exe').resolve(strict=True)


def snapshot(pid):
    rows = []
    for task in Path('/proc', str(pid), 'task').glob('*'):
        try:
            tid = int(task.name)
            rows.append(dict(tid=tid, name=(task/'comm').read_text().strip(),
                             stat=(task/'stat').read_text(),
                             schedstat=(task/'schedstat').read_text().strip(),
                             wchan=(task/'wchan').read_text().strip(),
                             cpus=sorted(os.sched_getaffinity(tid)),
                             policy=os.sched_getscheduler(tid),
                             priority=os.sched_getparam(tid).sched_priority))
            if tid == pid:
                # Identify a main-thread fsync without stopping the process.
                try:
                    call = (task/'syscall').read_text().strip()
                    rows[-1]['syscall'] = call
                    parts = call.split()
                    sync_calls = {'aarch64': {'82', '83'}, 'x86_64': {'74', '75'}}
                    if len(parts) > 1 and parts[0] in sync_calls.get(platform.machine(), set()):
                        fd = int(parts[1], 0)
                        rows[-1]['sync_fd_target'] = str((Path('/proc', str(pid), 'fd')/str(fd)).resolve(strict=True))
                except (OSError, ValueError):
                    pass
        except (OSError, ProcessLookupError):
            continue
    return rows


def capture(pid, expected, pidfd, output, timeout):
    # An open pidfd and start-tick/executable checks reject vanished/reused PIDs.
    if select.select([pidfd], [], [], 0)[0] or identity(pid) != expected:
        return 'target_changed'
    command = ['gdb', '-nx', '-nh', '-q', '-batch',
               '-iex', 'set auto-load off', '-ex', 'set pagination off',
               '-ex', 'set print thread-events off', '-ex', 'attach '+str(pid),
               '-ex', 'info threads', '-ex', 'thread apply all bt 16',
               '-ex', 'detach', '-ex', 'quit']
    with output.open('x') as stream:
        try:
            result = subprocess.run(command, stdout=stream, stderr=subprocess.STDOUT,
                                    stdin=subprocess.DEVNULL, timeout=timeout)
            return result.returncode
        except subprocess.TimeoutExpired:
            # Killing an attaching debugger can leave its target group-stopped.
            # Do not turn a diagnostic timeout into an application hang.
            if not select.select([pidfd], [], [], 0)[0]:
                try:
                    if identity(pid) == expected:
                        signal.pidfd_send_signal(pidfd, signal.SIGCONT)
                except (OSError, ProcessLookupError):
                    pass
            return 'gdb_timeout'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--pid', type=int, required=True)
    parser.add_argument('--executable', type=Path, required=True)
    parser.add_argument('--timing-file', type=Path, required=True)
    parser.add_argument('--output-directory', type=Path, required=True)
    parser.add_argument('--stall-seconds', type=float, default=1.5)
    parser.add_argument('--gdb-timeout', type=float, default=2)
    parser.add_argument('--duration', type=float, default=600)
    parser.add_argument('--capture-now', action='store_true',
                        help='Capture one baseline stack, then exit')
    parser.add_argument('--no-debugger', action='store_true',
                        help='Only observe thread state; never stop the target')
    args = parser.parse_args()
    if args.pid <= 1 or min(args.stall_seconds, args.gdb_timeout, args.duration) <= 0:
        parser.error('PID and timeouts must be positive')
    if args.stall_seconds + args.gdb_timeout >= 4.5:
        parser.error('Capture must finish before the five-second watchdog threshold')
    expected = identity(args.pid)
    if expected[0] != os.getuid() or expected[2] != args.executable.resolve(strict=True):
        parser.error('Target must be this user\'s exact isolated executable')
    args.output_directory.mkdir(parents=True, exist_ok=False)
    pidfd = os.pidfd_open(args.pid)
    try:
        if identity(args.pid) != expected:
            parser.error('Target changed during startup')
        with (args.output_directory/'samples.jsonl').open('x') as log:
            start = time.monotonic()
            last_change = start
            last_size = None
            armed = False
            captured = False
            while time.monotonic()-start < args.duration:
                if select.select([pidfd], [], [], 0)[0]:
                    break
                try:
                    if identity(args.pid) != expected:
                        break
                except (OSError, ProcessLookupError):
                    break
                now = time.monotonic()
                try:
                    size = args.timing_file.stat().st_size
                    if size != last_size:
                        last_change = now
                        armed = last_size is not None and size > last_size
                        last_size = size
                except FileNotFoundError:
                    pass
                row = dict(time_ns=time.monotonic_ns(), pid=args.pid,
                           csv_stale_seconds=now-last_change, armed=armed,
                           threads=snapshot(args.pid))
                log.write(json.dumps(row)+'\n'); log.flush()
                # Never attach while a thread is in uninterruptible kernel I/O,
                # or after a delayed observer has used up the watchdog budget.
                states = [t['stat'].rpartition(') ')[2].split()[0] for t in row['threads']]
                capture_allowed = args.capture_now or now-last_change < 4.5-args.gdb_timeout
                if not args.no_debugger and 'D' not in states and capture_allowed and not captured and (args.capture_now or (armed and now-last_change >= args.stall_seconds)):
                    print('Capturing bounded GDB stacks; performance data during attachment is invalid.', flush=True)
                    result = capture(args.pid, expected, pidfd,
                                     args.output_directory/'stacks.txt', args.gdb_timeout)
                    log.write(json.dumps(dict(event='stack_capture', time_ns=time.monotonic_ns(), result=result))+'\n')
                    log.flush(); captured = True
                    print('Stack capture result: '+str(result), flush=True)
                    if args.capture_now:
                        break
                time.sleep(.2)
    finally:
        os.close(pidfd)


if __name__ == '__main__':
    main()
