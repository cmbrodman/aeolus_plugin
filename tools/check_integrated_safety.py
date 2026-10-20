#!/usr/bin/env python3
# Copyright (C) 2026 Aeolus fork contributors
# SPDX-License-Identifier: GPL-3.0-or-later
"""Silent Linux device checks for automatic safety in a TEST-ENABLED standalone.

Pass only test-owned data/settings. No MIDI, scheduling, IRQ or system changes.
The second run deliberately SIGSTOPs and lets the owned companion terminate it.
"""
import argparse
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time

sys.path.insert(0,str(Path(__file__).resolve().parent/'claude_panic'))
from check_recovery import close_window
from aeolus_panic_client import PanicClient


def wait(predicate, seconds=40):
    deadline=time.monotonic()+seconds
    while time.monotonic()<deadline:
        value=predicate()
        if value:return value
        time.sleep(.05)
    raise TimeoutError('Integrated safety check timed out')


def runtime_for(pid,previous=None):
    for path in Path('/dev/shm').glob('aeolus-safety-*'):
        if path==previous:continue
        try:
            if path.stat().st_uid==os.getuid() and int((path/'ready').read_text())==pid:
                return path
        except (OSError,ValueError):pass
    return None


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--executable',type=Path,required=True)
    p.add_argument('--test-data',type=Path,required=True)
    p.add_argument('--test-settings',type=Path,required=True)
    p.add_argument('--desktop-environment',type=Path,required=True)
    p.add_argument('--output',type=Path,required=True)
    a=p.parse_args();binary=a.executable.resolve(strict=True)
    image=binary.read_bytes()
    assert b'Test standalone requires an absolute AEOLUS_TEST_SETTINGS_DIRECTORY' in image, 'Refusing a binary without test preference isolation'
    assert b'Tests require an absolute AEOLUS_TEST_DATA_DIRECTORY' in image, 'Refusing a binary without test organ isolation'
    assert a.test_data.is_absolute() and a.test_settings.is_absolute()
    assert a.test_data.is_dir() and a.test_settings.is_dir()
    a.output.mkdir(exist_ok=False,parents=True)
    env=os.environ.copy();env.update(json.loads(a.desktop_environment.read_text()))
    env.update(AEOLUS_TEST_DATA_DIRECTORY=str(a.test_data),AEOLUS_TEST_SETTINGS_DIRECTORY=str(a.test_settings),
        AEOLUS_AUTO_SAFETY='1',AEOLUS_PANIC_PANEL='0',AEOLUS_DSP_WORKERS='3',AEOLUS_MAX_VOICES='1024')
    for name in ['AEOLUS_PANIC_SOCKET','AEOLUS_AUDIO_RT_PRIORITY','AEOLUS_AUDIO_CPUS',
                 'AEOLUS_DSP_WORKER_CPUS','AEOLUS_CONTROL_CPUS','AEOLUS_CONTROL_RT_PRIORITY','LD_PRELOAD',
                 'AEOLUS_DSP_GROUP_SIZE','AEOLUS_DSP_BATCH_QUANTA','AEOLUS_BENCHMARK_FULL_VOICE_CLEAR']:
        env.pop(name,None)
    os.environ.update({key:env[key] for key in ['DISPLAY','XAUTHORITY'] if key in env})
    for mode in ['healthy-reset','frozen']:
        with tempfile.TemporaryDirectory(prefix='aeolus-device-check-',dir='/dev/shm') as tmp:
            ram=Path(tmp);env['AEOLUS_TIMING_FILE']=str(ram/'timing.csv')
            with (ram/'app.log').open('x') as stream:
                app=subprocess.Popen([str(binary)],env=env,cwd=binary.parent,
                    stdout=stream,stderr=subprocess.STDOUT,start_new_session=True)
            client=None;runtime=None
            try:
                runtime=wait(lambda:runtime_for(app.pid))
                client=PanicClient(runtime/'control.sock')
                def probe():
                    client.send('HEALTH');client.send('STATUS');time.sleep(.03);client.poll()
                    return client.audio_completed is not None and client.audio_completed>0
                wait(probe)
                info={'mode':mode,'pid':app.pid,'notes_sent':False,'runtime':str(runtime),'completed':client.audio_completed}
                if mode=='healthy-reset':
                    client.panic();wait(lambda:(client.send('STATUS'),client.poll(),client.completed())[-1])
                    assert client.muted
                    client.send('RESUME')
                    wait(lambda:(probe() and not client.muted and client.audio_state==1))
                    before=runtime;client.send('RESET_TEST');client.close();client=None
                    runtime=wait(lambda:runtime_for(app.pid,before))
                    wait(lambda:not before.exists())
                    assert app.poll() is None,'old guardian killed the replacement processor'
                    client=PanicClient(runtime/'control.sock');wait(probe)
                    info['replacement_runtime']=str(runtime)
                    close_window(app.pid,env)
                    app.wait(timeout=40)
                    assert app.returncode==0,app.returncode
                    wait(lambda:not runtime.exists())
                    info['normal_close']=True
                else:
                    fd=os.pidfd_open(app.pid)
                    try:signal.pidfd_send_signal(fd,signal.SIGSTOP)
                    finally:os.close(fd)
                    app.wait(timeout=20)
                    assert app.returncode==-signal.SIGKILL,app.returncode
                    wait(lambda:not runtime.exists())
                    reports=sorted((a.test_data/'SafetyReports').glob('safety-*.jsonl'))
                    assert reports,'no post-exit recovery report'
                    events=[json.loads(line)['event'] for line in reports[-1].read_text().splitlines()]
                    assert 'panic_sigkill_sent' in events,events
                    info['watchdog_events']=events
                info['exit_code']=app.returncode
                (a.output/(mode+'.json')).write_text(json.dumps(info,indent=2))
                (a.output/(mode+'.app.log')).write_bytes((ram/'app.log').read_bytes())
                print(json.dumps(info),flush=True)
            finally:
                if client is not None:client.close()
                if app.poll() is None:
                    app.kill();app.wait(timeout=5)
                (a.output/(mode+'.app.log')).write_bytes((ram/'app.log').read_bytes())
    print('AUTOMATIC SAFETY / EXPLICIT RESUME / PROCESSOR REPLACEMENT / FREEZE RECOVERY PASSED',flush=True)

if __name__=='__main__':main()
