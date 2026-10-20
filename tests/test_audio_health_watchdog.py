"""Audio progress must be independent of CSV, GUI progress and device suspension."""
import json
import os
import signal
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import unittest

WATCHDOG = Path(__file__).resolve().parents[1]/'tools/vim4_test_watchdog.py'
SERVER = r'''
import signal,socket,sys,time
from pathlib import Path
sock=socket.socket(socket.AF_UNIX,socket.SOCK_DGRAM);sock.bind(sys.argv[1])
mode=sys.argv[3];start=time.monotonic();epoch=1 if mode=='manual-stuck' else 0;ack=0;muted=0
Path(sys.argv[2]).write_text('sequence,start_ns,duration_ns,sample_rate,frames,voices,native_xruns\n0,'+str(time.monotonic_ns())+',1,44100,512,0,0\n')
while True:
 command,client=sock.recvfrom(256);elapsed=time.monotonic()-start
 complete=int(min(elapsed,.2)*100) if mode=='audio-stuck' else int(elapsed*100)+1
 if mode in ('inactive','reprepare-stuck','prepare-stuck','inactive-inflight'):complete=1
 if mode=='not-started':complete=0
 if mode=='long-resume':complete=1 if elapsed<.45 else int(elapsed*100)
 if command==b'HEALTH':
  if mode=='legacy':continue
  active=2 if mode in ('prepare-stuck','prepare-with-audio') or (mode=='long-resume' and elapsed<.30) else 0 if mode in ('inactive','reprepare-stuck','inactive-inflight','not-started') else 1
  gui=int(elapsed*10) if mode in ('audio-stuck','prepare-stuck','inactive-inflight','long-resume') else 1
  sock.sendto(('HEALTH 2 %d %d %d %d'%(complete+(mode=='inactive-inflight'),complete,active,gui)).encode(),client);continue
 if command.startswith(b'PANIC '):
  epoch+=1
  if mode not in ('audio-stuck','reprepare-stuck','prepare-stuck','inactive-inflight','manual-stuck'):ack=epoch;muted=1
 reply=('STATUS %d %d %d'%(epoch,ack,2 if mode=='reprepare-stuck' else muted)).encode()
 if command.startswith(b'PANIC '):reply+=b' '+command.split()[1]
 sock.sendto(reply,client)
'''


@unittest.skipUnless(sys.platform=='linux', 'Linux Unix sockets and pidfds')
class HealthWatchdogTests(unittest.TestCase):
    def launch(self, mode, extra=()):
        tmp=tempfile.TemporaryDirectory();self.addCleanup(tmp.cleanup)
        root=Path(tmp.name);path=root/'control.sock';timing=root/'timing.csv';log=root/'watch.jsonl'
        child=subprocess.Popen([sys.executable,'-c',SERVER,str(path),str(timing),mode],stderr=subprocess.DEVNULL)
        self.addCleanup(self.stop,child)
        deadline=time.monotonic()+3
        while not path.exists() and time.monotonic()<deadline:time.sleep(.01)
        watcher=subprocess.Popen([sys.executable,str(WATCHDOG),'--pid',str(child.pid),
            '--executable',str(Path(sys.executable).resolve()),'--timing-file',str(timing),
            '--log',str(log),'--cpus',','.join(map(str,os.sched_getaffinity(0))),
            '--panic-socket',str(path),*(['--require-audio-heartbeat'] if mode!='legacy' else []),'--allow-stop',
            '--stalled-after','.25','--panic-timeout','.15','--poll','.025',
            '--startup-timeout','.6','--resume-timeout','.3',*extra],stdout=subprocess.DEVNULL,stderr=subprocess.PIPE)
        self.addCleanup(self.stop,watcher)
        return child,watcher,log

    @staticmethod
    def stop(process):
        if process.poll() is None:process.kill()
        process.wait(timeout=5)
        if process.stderr is not None:process.stderr.close()

    @staticmethod
    def events(log):
        return [json.loads(l)['event'] for l in log.read_text().splitlines()] if log.exists() else []

    def test_stale_csv_and_gui_do_not_panic_progressing_audio(self):
        child,watcher,log=self.launch('healthy')
        time.sleep(.8)
        self.assertIsNone(child.poll());self.assertIsNone(watcher.poll())
        events=self.events(log)
        self.assertIn('armed',events);self.assertIn('ui_progress_stalled',events)
        self.assertFalse(json.loads(log.read_text().splitlines()[0])['csv_monitoring'])
        self.assertNotIn('panic_requested',events)

    def test_safety_loop_never_opens_a_blocked_csv(self):
        child,watcher,log=self.launch('healthy')
        deadline=time.monotonic()+2
        while 'armed' not in self.events(log) and time.monotonic()<deadline:time.sleep(.01)
        self.assertIn('armed',self.events(log))
        timing=log.parent/'timing.csv';timing.unlink();os.mkfifo(timing)
        time.sleep(.5)
        self.assertNotIn('panic_requested',self.events(log))
        child.terminate();child.wait(timeout=2)
        # Reading the FIFO would block open() forever even after target exit.
        self.assertEqual(watcher.wait(timeout=2),0)

    def test_fresh_inactive_device_is_not_treated_as_hung_audio(self):
        child,watcher,log=self.launch('inactive')
        time.sleep(.8)
        self.assertIsNone(child.poll());self.assertIn('audio_inactive',self.events(log))
        self.assertNotIn('panic_requested',self.events(log))

    def test_audio_stall_recovers_even_when_control_and_gui_respond(self):
        child,watcher,log=self.launch('audio-stuck')
        self.assertEqual(watcher.wait(timeout=4),0)
        self.assertEqual(child.wait(timeout=2),-15)
        events=self.events(log)
        for expected in ('audio_progress_stalled','panic_requested','panic_unacknowledged','panic_sigterm_sent'):
            self.assertIn(expected,events)

    def test_legacy_endpoint_never_arms_or_blindly_terminates(self):
        child,watcher,log=self.launch('legacy')
        self.assertEqual(watcher.wait(timeout=3),2)
        self.assertIsNone(child.poll())
        self.assertIn('startup_without_telemetry',self.events(log))
        self.assertNotIn('armed',self.events(log));self.assertNotIn('panic_requested',self.events(log))

    def test_reprepare_timeout_is_not_hidden_by_inactive_state(self):
        child,watcher,log=self.launch('reprepare-stuck')
        self.assertEqual(watcher.wait(timeout=4),0)
        self.assertEqual(child.wait(timeout=2),-15)
        self.assertIn('panic_sigterm_sent',self.events(log))

    def test_prepare_outside_explicit_resume_is_bounded(self):
        child,watcher,log=self.launch('prepare-stuck')
        self.assertEqual(watcher.wait(timeout=4),0)
        self.assertEqual(child.wait(timeout=2),-15)
        self.assertIn('panic_sigterm_sent',self.events(log))

    def test_inactive_flag_does_not_exempt_inflight_callback(self):
        child,watcher,log=self.launch('inactive-inflight')
        self.assertEqual(watcher.wait(timeout=4),0)
        self.assertEqual(child.wait(timeout=2),-15)
        self.assertIn('audio_progress_stalled',self.events(log))

    def test_completed_prepare_gets_a_fresh_callback_window(self):
        child,watcher,log=self.launch('long-resume')
        time.sleep(.8)
        self.assertIsNone(child.poll());self.assertIsNone(watcher.poll())
        self.assertIn('audio_reprepare_finished',self.events(log))
        self.assertNotIn('panic_requested',self.events(log))

    def test_slow_prepare_flag_does_not_override_observed_audio_progress(self):
        child,watcher,log=self.launch('prepare-with-audio')
        time.sleep(.8)
        self.assertIsNone(child.poll());self.assertIsNone(watcher.poll())
        self.assertNotIn('panic_requested',self.events(log))

    def test_frozen_receiver_is_not_exempted_by_its_old_inactive_reply(self):
        child,watcher,log=self.launch('inactive')
        deadline=time.monotonic()+2
        while 'audio_inactive' not in self.events(log) and time.monotonic()<deadline:time.sleep(.01)
        self.assertIn('audio_inactive',self.events(log))
        descriptor=os.pidfd_open(child.pid)
        try:signal.pidfd_send_signal(descriptor,signal.SIGSTOP)
        finally:os.close(descriptor)
        self.assertEqual(watcher.wait(timeout=5),0)
        self.assertEqual(child.wait(timeout=2),-9)
        self.assertIn('panic_sigkill_sent',self.events(log))

    def test_production_waits_for_first_audio_without_blind_recovery(self):
        child,watcher,log=self.launch('not-started',['--wait-for-audio'])
        time.sleep(.85)
        self.assertIsNone(watcher.poll())
        self.assertIsNone(child.poll())
        self.assertNotIn('armed',self.events(log))
        self.assertNotIn('panic_requested',self.events(log))

    def test_manual_unacknowledged_panic_recovers_even_with_progressing_audio(self):
        child,watcher,log=self.launch('manual-stuck')
        watcher.wait(timeout=4)
        self.assertIsNotNone(child.poll())
        events=self.events(log)
        self.assertIn('manual_panic_unacknowledged',events)
        self.assertIn('panic_sigterm_sent',events)
        self.assertNotIn('audio_progress_stalled',events)

    def test_ready_marker_and_bounded_shutdown_grace(self):
        with tempfile.TemporaryDirectory() as tmp:
            ready=Path(tmp)/'ready';closing=Path(tmp)/'closing'
            child,watcher,log=self.launch('csv-stuck',['--ready-file',str(ready),
                '--shutdown-file',str(closing),'--shutdown-timeout','.4'])
            deadline=time.monotonic()+2
            while not ready.exists() and time.monotonic()<deadline:time.sleep(.01)
            self.assertTrue(ready.exists())
            self.assertEqual(int(ready.read_text()),child.pid)
            closing.touch();os.kill(child.pid,signal.SIGSTOP)
            time.sleep(.2)
            self.assertNotIn('panic_requested',self.events(log))
            watcher.wait(timeout=6)
            self.assertIsNotNone(child.poll())
            self.assertIn('application_close_stalled',self.events(log))
            self.assertIn('panic_sigkill_sent',self.events(log))


if __name__=='__main__':unittest.main()
