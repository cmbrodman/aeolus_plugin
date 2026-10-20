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
import collections
import shutil
import time


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


def identify_audio_thread(pid, placement, prefix, log):
    candidates = [Path('/usr/lib/linux-tools-6.8.0-147/perf')]
    candidates += sorted(Path('/usr/lib/linux-tools').glob('*/perf'), reverse=True)
    if shutil.which('perf'):
        candidates.append(Path(shutil.which('perf')))
    perf = next((path for path in candidates if path.is_file()), None)
    if perf is None:
        raise RuntimeError('No perf executable available to identify the unnamed ALSA thread.')
    recording = prefix.with_suffix('.setup-perf.data')
    subprocess.run([str(perf), 'record', '-q', '-F', '999', '-e', 'cpu-clock:u',
                    '-t', ','.join(placement), '-o', str(recording), '--', 'sleep', '4'],
                   stdout=log, stderr=log, check=True)
    script = subprocess.check_output([str(perf), 'script', '-i', str(recording), '-F', 'tid,ip,sym'], text=True)
    prefix.with_suffix('.setup-perf.txt').write_text(script)
    counts = collections.Counter()
    symbols = ['aeolus::Engine::process', 'AeolusAudioProcessor::processBlock',
               'AeolusAudioProcessor::renderRange', 'aeolus::CallbackProfiler::record',
               'juce::AudioProcessorPlayer::audioDeviceIOCallbackWithContext']
    for line in script.splitlines():
        fields = line.split()
        if fields and fields[0].isdigit() and any(symbol in line for symbol in symbols):
            counts[int(fields[0])] += 1
    if len(counts) != 1:
        raise RuntimeError('Audio thread identification was ambiguous: ' + str(dict(counts)))
    return next(iter(counts))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('mode', choices=['candidate', 'serial', 'serial-large', 'legacy'], nargs='?', default='candidate')
    parser.add_argument('--bundle', type=Path, default=Path(__file__).resolve().parent)
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
    environment['AEOLUS_TEST_DATA_DIRECTORY'] = str(bundle / 'data')
    environment['XDG_CONFIG_HOME'] = str(bundle / 'config')
    environment['AEOLUS_TEST_SETTINGS_DIRECTORY'] = str(bundle / 'config')
    environment['AEOLUS_TIMING_FILE'] = str(timing)
    if args.mode != 'legacy':
        environment['AEOLUS_VOICE_WORKERS'] = '3' if args.mode == 'candidate' else '0'
        environment['AEOLUS_VOICE_JOB_QUANTA'] = '1' if args.mode == 'serial' else '8'
        environment['AEOLUS_PERFORMANCE_CPU_BASE'] = '0'
    metadata = {'mode': args.mode, 'binary_sha256': hashlib.sha256(binary.read_bytes()).hexdigest(),
                'isolated_data': str(bundle / 'data'),
                'isolated_settings': str(bundle / 'config'), 'timing_file': str(timing)}
    with prefix.with_suffix('.app.log').open('xb') as log:
        process = subprocess.Popen([str(binary)], cwd=binary.parent, env=environment,
                                   stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
        metadata['pid'] = process.pid
        ready = False
        deadline = time.monotonic() + 90
        while process.poll() is None:
            placement = thread_placement(process.pid)
            audio = [int(tid) for tid, item in placement.items()
                     if item['name'].startswith('JUCE v') and ': AL' in item['name']]
            if not audio and metadata.get('audio_tid') in [int(tid) for tid in placement]:
                audio = [metadata['audio_tid']]
            if not audio and timing.exists() and timing.stat().st_size > 256:
                try:
                    audio = [identify_audio_thread(process.pid, placement, prefix, log)]
                    metadata['setup_perf_only'] = True
                except (RuntimeError, subprocess.CalledProcessError) as error:
                    print(str(error), flush=True)
                    metadata['affinity_error'] = str(error)
                    deadline = float('inf')
                    # Keep the test window available for normal user closure;
                    # never claim readiness after an ambiguous identification.
                    process.wait()
                    break
            if len(audio) == 1:
                try:
                    os.sched_setaffinity(audio[0], {0})
                except ProcessLookupError:
                    continue
                placement = thread_placement(process.pid)
                metadata['audio_tid'] = audio[0]
            verified = (len(audio) == 1 and placement.get(str(audio[0]), {}).get('affinity') == [0]
                        and placement.get(str(process.pid), {}).get('affinity') == [4, 5, 6, 7])
            if args.mode == 'candidate':
                verified &= all(any(item['name'] == 'AeolusVoice' + str(lane) and item['affinity'] == [lane]
                                    for item in placement.values()) for lane in [1, 2, 3])
            metadata['threads'] = placement
            if timing.exists() and 'first_callback' not in metadata:
                with timing.open() as stream:
                    for row in csv.DictReader(stream):
                        if row.get('sample_rate') and row.get('frames'):
                            metadata['first_callback'] = {'sample_rate': row['sample_rate'], 'frames': row['frames']}
                            break
            metadata['affinity_verified'] = bool(verified)
            prefix.with_suffix('.placement.json').write_text(json.dumps(metadata, indent=2) + '\n')
            if verified and 'first_callback' in metadata and not ready:
                ready = True
                metadata['ready_ns'] = time.monotonic_ns()
                print('READY: audio core 0; voice workers 1–3 in candidate mode; UI/background 4–7.', flush=True)
                print('Actual audio: ' + str(metadata['first_callback']) + '\nLogs: ' + str(prefix), flush=True)
            if not ready and time.monotonic() >= deadline:
                print('Startup/ALSA affinity was not verified. Close the test window; this run is unsuitable for comparison.', flush=True)
                deadline = float('inf')
            time.sleep(0.5)
        metadata['exit_code'] = process.returncode
        prefix.with_suffix('.placement.json').write_text(json.dumps(metadata, indent=2) + '\n')
        print('Test application closed. Logs: ' + str(prefix), flush=True)
        return process.returncode or (0 if ready else 1)


if __name__ == '__main__':
    raise SystemExit(main())
