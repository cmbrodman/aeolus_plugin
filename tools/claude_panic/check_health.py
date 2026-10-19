#!/usr/bin/env python3
"""Silent VIM4 device check: blocked logger must not block UI/audio or recovery."""
import argparse
import json
import os
from pathlib import Path
import signal
import subprocess
import time

from aeolus_panic_client import PanicRecovery
from check_recovery import close_window


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bundle',type=Path,required=True)
    parser.add_argument('--desktop-environment',type=Path,required=True)
    parser.add_argument('--fault-library',type=Path,required=True)
    args=parser.parse_args();bundle=args.bundle.resolve()
    env=os.environ.copy();env.update(json.loads(args.desktop_environment.read_text()))
    os.environ.update(env) # Xlib reads the process environment, not Popen's env.
    os.sched_setaffinity(0,{6,7})
    label='health-'+str(time.time_ns())
    # The marker is tiny, exclusively created, and outside the delayed log path.
    marker=Path('/dev/shm')/(label+'-writer-paused')
    env['LD_PRELOAD']=str(args.fault_library.resolve(strict=True))
    env['AEOLUS_TEST_LOGGER_DELAY_MS']='30000'
    env['AEOLUS_TEST_LOGGER_DELAY_MARKER']=str(marker)
    start=time.time_ns();output=bundle/(label+'.launch.log')
    with output.open('x') as log:
        launcher=subprocess.Popen(['python3',str(bundle/'run-test.py')],env=env,
            stdin=subprocess.DEVNULL,stdout=log,stderr=subprocess.STDOUT)
    row=None;placement=None;deadline=time.monotonic()+60
    while time.monotonic()<deadline and launcher.poll() is None:
        for path in (bundle/'logs').glob('*.placement.json'):
            if path.stat().st_mtime_ns<start:continue
            try:r=json.loads(path.read_text())
            except ValueError:continue
            if r.get('ready_ns') and r.get('panic_control_verified'):
                row=r;placement=path;break
        if row:break
        time.sleep(.1)
    assert row,'Independent safety failed to arm before CSV availability; inspect '+str(output)
    writers=[t for t in row['threads'].values() if t['name']=='aeolus-log']
    assert len(writers)==1 and writers[0]['affinity']==[4,5,6,7] and writers[0]['scheduler']==os.SCHED_OTHER, writers
    recovery=PanicRecovery(row['pid'],bundle/'Standalone/Aeolus-Profile',row['panic_socket'],
        lambda event,**data:None,allow_stop=True)
    try:
        timing=Path(row['timing_file'])
        assert marker.exists(),'Writer fault injection did not run'
        assert timing.stat().st_size==0,'CSV already written before protected startup'
        def probe():
            recovery.client.send('HEALTH');recovery.client.send('STATUS')
            time.sleep(.05);recovery.client.poll()
            return dict(time_ns=time.monotonic_ns(),completed=recovery.client.audio_completed,
                        gui=recovery.client.gui_progress,active=recovery.client.audio_active)
        before=probe();assert before['completed'] is not None and before['completed']>0
        print('Safety armed while CSV is empty; observing more than five seconds of blocked writer.',flush=True)
        time.sleep(6.5)
        after=probe()
        assert recovery.alive() and after['completed']>before['completed'],'Audio stopped during blocked logging'
        assert after['gui']>before['gui'],'GUI stopped during blocked logging'
        assert timing.stat().st_size==0,'Fault did not keep CSV blocked through the observation'
        watcher=Path(row['watchdog_log'])
        records=[json.loads(l) for l in watcher.read_text().splitlines()]
        events=[r['event'] for r in records]
        assert 'armed' in events and not records[0]['csv_monitoring'],events
        assert 'panic_requested' not in events,'Stale CSV incorrectly triggered audio recovery'
        deadline=time.monotonic()+40
        while timing.stat().st_size==0 and recovery.alive() and time.monotonic()<deadline:probe()
        assert recovery.alive() and timing.stat().st_size>0,'Writer did not recover'
        recovery.request();deadline=time.monotonic()+3
        while recovery.state=='pending' and time.monotonic()<deadline:
            recovery.tick();time.sleep(.025)
        assert recovery.state=='muted','Healthy panic was not acknowledged'
        epoch=recovery.client.requested
        for _ in range(3):
            recovery.client.send('PANIC '+recovery.client.nonce)
            probe()
        assert recovery.client.requested==epoch,'Repeated panic nonce created extra requests'
        recovery.client.send('RESUME');deadline=time.monotonic()+25
        while recovery.alive() and time.monotonic()<deadline:
            probe()
            if not recovery.client.muted:break
        assert recovery.alive() and not recovery.client.muted,'Explicit Resume failed'
        close_window(row['pid'],os.environ)
        assert launcher.wait(timeout=30)==0,'Normal close failed'
        latest=json.loads(placement.read_text())
        footer=timing.read_text().splitlines()[-1]
        assert footer=='# dropped_total=0','Blocked-writer capture was incomplete: '+footer
        watcher=timing.with_suffix('.watchdog.jsonl') # persisted after launcher exit
        events=[json.loads(l)['event'] for l in watcher.read_text().splitlines()]
        proof=dict(notes_sent=False,writer_delay_ms=30000,
                   safety_armed_with_empty_csv=True,before=before,after=after,
                   csv_reading_disabled_in_safety_loop=True,panic_nonce_retries_idempotent=True,
                   panic_and_resume_passed=True,normal_exit=True,
                   writer_affinity=writers[0]['affinity'],writer_scheduler=writers[0]['scheduler'],
                   telemetry_footer=footer,
                   actual_audio=latest.get('first_callback'),watchdog_events=events)
        assert 'panic_sigterm_sent' not in events and 'audio_progress_stalled' not in events,events
        (bundle/(label+'-proof.json')).write_text(json.dumps(proof,indent=2)+'\n')
        print(json.dumps(proof),flush=True)
        print('BLOCKED LOGGER, INDEPENDENT AUDIO/UI HEARTBEATS, PANIC/RESUME PASSED',flush=True)
    finally:
        if recovery.alive():
            signal.pidfd_send_signal(recovery.pidfd,signal.SIGTERM)
            try:launcher.wait(timeout=3)
            except subprocess.TimeoutExpired:
                if recovery.alive():signal.pidfd_send_signal(recovery.pidfd,signal.SIGKILL)
                launcher.wait(timeout=10)
        recovery.close()


if __name__=='__main__':main()
