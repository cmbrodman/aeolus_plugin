#!/usr/bin/env python3
"""Launch the isolated ARM64 voice-job test bundle on the known VIM4 layout."""
import argparse
import csv
import hashlib
import json
import os
from pathlib import Path
import platform
import subprocess
import time
import tempfile
import shutil


def thread_placement(pid):
    result = {}
    for task in Path('/proc', str(pid), 'task').glob('*'):
        try:
            result[task.name] = {
                'name': (task / 'comm').read_text().strip(),
                'affinity': sorted(os.sched_getaffinity(int(task.name))),
            }
        except (FileNotFoundError, ProcessLookupError):
            pass
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('mode', choices=['candidate', 'serial-large', 'legacy'], nargs='?', default='candidate')
    parser.add_argument('--bundle', type=Path, default=Path(__file__).resolve().parent)
    parser.add_argument('--no-panel', action='store_true', help='Headless fault-injection checks only')
    args = parser.parse_args()
    bundle = args.bundle.resolve()
    binary = bundle / 'Standalone/Aeolus-Profile'
    if platform.machine() not in ('aarch64', 'arm64'):
        parser.error('This launcher targets the ARM64 Khadas VIM4.')
    if not binary.is_file() or not (bundle / 'data/organ_config.json').is_file():
        parser.error('Expected Standalone/Aeolus-Profile and isolated data/organ_config.json in the bundle.')
    for name in ['Aeolus', 'Aeolus-Profile']:
        if subprocess.run(['pgrep', '-x', name], stdout=subprocess.DEVNULL).returncode == 0:
            parser.error('Close the existing ' + name + ' window normally first.')
    os.sched_setaffinity(0, {4, 5, 6, 7})
    logs = bundle / 'logs'
    logs.mkdir(exist_ok=True)
    stamp = time.strftime('%Y%m%d-%H%M%S') + '-' + str(time.time_ns() % 1_000_000_000)
    prefix = logs / (args.mode + '-' + stamp)
    timing = prefix.with_suffix('.csv')
    environment = os.environ.copy()
    for key in ['AEOLUS_SPATIAL_WORKERS', 'AEOLUS_VOICE_WORKERS', 'AEOLUS_VOICE_JOB_QUANTA',
                'AEOLUS_PERFORMANCE_CPU_BASE', 'AEOLUS_VOICE_TEST_DELAY_US']:
        environment.pop(key, None)
    control_directory = Path(tempfile.mkdtemp(prefix='aeolus-safety-', dir='/dev/shm'))
    # Only the background profiler writes to disk during playback. All control
    # logs/metadata/stdout live in RAM; persist them after audio has stopped.
    suffixes = ['.app.log', '.placement.json', '.watchdog.jsonl', '.helper-0.log']
    if not args.no_panel: suffixes += ['.panel.jsonl', '.helper-1.log']
    def sidecar(suffix):
        return control_directory / (prefix.name + suffix)
    sidecar('.placement.json').write_text('{}\n')
    for suffix in suffixes:
        prefix.with_suffix(suffix).symlink_to(sidecar(suffix))
    environment['AEOLUS_PANIC_SOCKET'] = str(control_directory / 'control.sock')
    environment['AEOLUS_TEST_DATA_DIRECTORY'] = str(bundle / 'data')
    environment['XDG_CONFIG_HOME'] = str(bundle / 'config')
    environment['AEOLUS_TEST_SETTINGS_DIRECTORY'] = str(bundle / 'config')
    environment['AEOLUS_TIMING_FILE'] = str(timing)
    environment.pop('AEOLUS_PER_VOICE_RANDOM', None)
    environment.pop('AEOLUS_BENCHMARK_FULL_VOICE_CLEAR', None)
    environment['AEOLUS_AUDIO_RT_PRIORITY'] = '70'
    environment['AEOLUS_AUDIO_CPUS'] = '0'
    environment['AEOLUS_DSP_WORKER_CPUS'] = '1-3'
    environment['AEOLUS_MAX_VOICES'] = '1024'
    environment['AEOLUS_DSP_GROUP_SIZE'] = '8'
    environment['AEOLUS_DSP_BATCH_QUANTA'] = '32'
    if args.mode != 'legacy':
        environment['AEOLUS_DSP_WORKERS'] = '3' if args.mode == 'candidate' else '0'
    else:
        environment.pop('AEOLUS_DSP_WORKERS', None)
    metadata = {'mode': args.mode, 'binary_sha256': hashlib.sha256(binary.read_bytes()).hexdigest(),
                'isolated_data': str(bundle / 'data'),
                'panic_socket': environment['AEOLUS_PANIC_SOCKET'], 'runtime_directory': str(control_directory),
                'watchdog_log': str(sidecar('.watchdog.jsonl')), 'isolated_settings': str(bundle / 'config'), 'timing_file': str(timing)}
    with sidecar('.app.log').open('xb') as log:
        process = subprocess.Popen([str(binary)], cwd=binary.parent, env=environment,
                                   stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
        metadata['pid'] = process.pid
        ready = False
        helpers = []
        helpers_started = False
        helper_logs = []
        deadline = time.monotonic() + 90
        while process.poll() is None:
            placement = thread_placement(process.pid)
            for tid, item in placement.items():
                try:
                    item['scheduler'] = os.sched_getscheduler(int(tid))
                    item['priority'] = os.sched_getparam(int(tid)).sched_priority
                except ProcessLookupError:
                    pass
            audio = [int(tid) for tid,item in placement.items()
                     if item.get('scheduler') == os.SCHED_FIFO and item.get('priority') == 70
                     and item['affinity'] == [0]]
            verified = len(audio) == 1 and placement.get(str(process.pid), {}).get('affinity') == [4,5,6,7]
            if audio:
                metadata['audio_tid'] = audio[0]
            if args.mode == 'candidate':
                verified &= all(any(item['name'] == 'aeolus-dsp-' + str(lane)
                                    and item['affinity'] == [lane]
                                    and item.get('scheduler') == os.SCHED_FIFO and item.get('priority') == 70
                                    for item in placement.values()) for lane in [1,2,3])
            metadata['threads'] = placement
            metadata['affinity_verified'] = bool(verified)
            receiver_verified = any(item['name'] == 'aeolus-panic' and item['affinity'] == [4]
                                    and item.get('scheduler') == os.SCHED_FIFO and item.get('priority') == 1
                                    for item in placement.values())
            if verified and receiver_verified and not helpers_started:
                watcher_log = sidecar('.watchdog.jsonl')
                commands = [[str(bundle/'vim4_test_watchdog.py'), '--pid', str(process.pid),
                             '--executable', str(binary), '--timing-file', str(timing),
                             '--log', str(watcher_log), '--cpus', '4,6,7', '--rt-priority', '1',
                             '--panic-socket', environment['AEOLUS_PANIC_SOCKET'], '--allow-stop',
                             '--require-audio-heartbeat']]
                helper_logs = [watcher_log]
                if not args.no_panel:
                    panel_log = sidecar('.panel.jsonl')
                    commands.append([str(bundle/'vim4_panic_panel.py'), '--pid', str(process.pid),
                                     '--executable', str(binary), '--socket', environment['AEOLUS_PANIC_SOCKET'],
                                     '--log', str(panel_log), '--allow-stop'])
                    helper_logs.append(panel_log)
                for index, command in enumerate(commands):
                    with sidecar('.helper-'+str(index)+'.log').open('xb') as output:
                        helpers.append(subprocess.Popen(['python3', *command], env=environment,
                                       stdin=subprocess.DEVNULL, stdout=output, stderr=subprocess.STDOUT,
                                       start_new_session=True))
                helpers_started = True
                metadata['helper_pids'] = [helper.pid for helper in helpers]
            helper_ready = helpers_started and all(helper.poll() is None for helper in helpers)
            for index, path in enumerate(helper_logs):
                try:
                    entries = [json.loads(line) for line in path.read_text().splitlines()]
                    expected = 'started' if index == 0 else 'panel_started'
                    helper_ready &= any(row.get('event') == expected and row.get('scheduler') == os.SCHED_FIFO
                                        and row.get('priority') == 1 for row in entries)
                    if index == 0:
                        helper_ready &= any(row.get('event') == 'armed' for row in entries)
                except (FileNotFoundError, ValueError):
                    helper_ready = False
            metadata['panic_control_verified'] = bool(receiver_verified and helper_ready)
            if verified and receiver_verified and helper_ready and not ready:
                ready = True
                metadata['ready_ns'] = time.monotonic_ns()
                print('READY: FIFO 70 audio/worker cores 0-3; independent panic receiver/watchdog/panel on efficiency cores.', flush=True)
                print('Actual audio: ' + str(metadata.get('first_callback', 'awaiting background CSV; audio heartbeat verified'))
                      + '\nLogs: ' + str(prefix), flush=True)
            if not ready and time.monotonic() >= deadline:
                print('Startup/ALSA affinity was not verified. Close the test window; this run is unsuitable for comparison.', flush=True)
                deadline = float('inf')
            sidecar('.placement.json').write_text(json.dumps(metadata, indent=2) + '\n')
            if ready and timing.exists() and 'first_callback' not in metadata:
                with timing.open() as stream:
                    for row in csv.DictReader(stream):
                        if row.get('sample_rate') and row.get('frames'):
                            metadata['first_callback'] = {'sample_rate': row['sample_rate'], 'frames': row['frames']}
                            break
            time.sleep(0.5)
        for helper in helpers:
            try: helper.wait(timeout=5)
            except subprocess.TimeoutExpired:
                helper.terminate()
                try: helper.wait(timeout=3)
                except subprocess.TimeoutExpired: helper.kill(); helper.wait()
        metadata['exit_code'] = process.returncode
        sidecar('.placement.json').write_text(json.dumps(metadata, indent=2) + '\n')
        for suffix in suffixes:
            source, destination = sidecar(suffix), prefix.with_suffix(suffix)
            if not destination.is_symlink() or os.readlink(destination) != str(source):
                raise RuntimeError('Control log destination changed; preserving RAM evidence')
            destination.unlink()
            if source.exists():
                with source.open('rb') as incoming, destination.open('xb') as outgoing:
                    shutil.copyfileobj(incoming, outgoing)
                source.unlink()
        socket_path = control_directory / 'control.sock'
        if socket_path.exists(): socket_path.unlink()
        control_directory.rmdir()
        print('Test application closed. Logs: ' + str(prefix), flush=True)
        return process.returncode or (0 if ready else 1)


if __name__ == '__main__':
    raise SystemExit(main())
