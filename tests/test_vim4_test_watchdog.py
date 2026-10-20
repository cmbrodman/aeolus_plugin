"""Subprocess fault-injection tests; never target a real audio application."""
import json
import os
from pathlib import Path
import resource
import subprocess
import sys
import tempfile
import time
import unittest

WATCHDOG = Path(__file__).resolve().parents[1] / 'tools/vim4_test_watchdog.py'
CHILD = r'''
import os,sys,time,signal
if sys.argv[2]=='fifo':
 os.sched_setscheduler(0,os.SCHED_FIFO,os.sched_param(1))
if sys.argv[2]=='ignore-term':signal.signal(signal.SIGTERM,signal.SIG_IGN)
with open(sys.argv[1],'w') as f:
 f.write('sequence,start_ns,duration_ns,sample_rate,frames,voices,native_xruns\n');f.flush()
 for n in range(8):
  f.write(str(n)+','+str(time.monotonic_ns())+',1,44100,512,0,0\n');f.flush();time.sleep(.05)
 if sys.argv[2]=='resume':
  time.sleep(.6)
  for n in range(30):
   f.write(str(n+8)+','+str(time.monotonic_ns())+',1,44100,512,0,0\n');f.flush();time.sleep(.05)
 else:time.sleep(30)
'''


@unittest.skipUnless(sys.platform == 'linux', 'Linux process/scheduler supervision')
class WatchdogTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.children = []
        self.addCleanup(self.stop_children)

    def stop_children(self):
        for child in self.children:
            if child.poll() is None:
                child.kill()
            child.wait(timeout=5)

    def launch(self, mode, *options, executable=None):
        timing = self.root / 'timing.csv'
        child = subprocess.Popen([sys.executable, '-c', CHILD, str(timing), mode],
                                 stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        self.children.append(child)
        watcher = subprocess.Popen([
            sys.executable, str(WATCHDOG), '--pid', str(child.pid),
            '--executable', str(executable or Path(sys.executable).resolve()),
            '--timing-file', str(timing), '--log', str(self.root / 'watch.jsonl'),
            '--cpus', ','.join(map(str, sorted(os.sched_getaffinity(0)))),
            '--stalled-after', '.25', '--poll', '.03', '--startup-timeout', '3', *options],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        self.children.append(watcher)
        return child, watcher

    def events(self):
        path = self.root / 'watch.jsonl'
        return [json.loads(line) for line in path.read_text().splitlines()] if path.exists() else []

    def await_event(self, event):
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            rows = self.events()
            if any(row['event'] == event for row in rows):
                return rows
            time.sleep(.03)
        self.fail('Missing watchdog event: ' + event)

    def test_stall_then_resume_does_not_terminate(self):
        child, watcher = self.launch('resume')
        self.await_event('progress_resumed')
        self.assertIsNone(child.poll())
        self.assertEqual(watcher.wait(timeout=5), 0)
        names = [row['event'] for row in self.events()]
        self.assertIn('telemetry_stalled', names)
        self.assertIn('realtime_priority_removed', names)
        self.assertNotIn('sigterm_sent', names)

    def test_monitor_only_never_changes_scheduling(self):
        child, watcher = self.launch('stall', '--monitor-only')
        self.await_event('telemetry_stalled')
        self.assertIsNone(child.poll())
        self.assertNotIn('realtime_priority_removed', [x['event'] for x in self.events()])
        child.terminate()
        self.assertEqual(watcher.wait(timeout=5), 0)

    def test_wrong_executable_is_rejected(self):
        child, watcher = self.launch('stall', executable=Path('/bin/false'))
        self.assertNotEqual(watcher.wait(timeout=5), 0)
        self.assertIsNone(child.poll())
        self.assertEqual(self.events(), [])

    def test_explicit_termination_escalation(self):
        child, watcher = self.launch('ignore-term', '--terminate-after', '.5', '--kill-after', '.2')
        self.assertEqual(watcher.wait(timeout=5), 3)
        self.assertEqual(child.wait(timeout=5), -9)
        names = [x['event'] for x in self.events()]
        self.assertIn('sigterm_sent', names)
        self.assertIn('sigkill_sent', names)

    @unittest.skipUnless(resource.getrlimit(resource.RLIMIT_RTPRIO)[0] >= 1,
                         'Requires existing permission for FIFO priority 1')
    def test_removes_actual_fifo_priority(self):
        child, watcher = self.launch('fifo')
        rows = self.await_event('realtime_priority_removed')
        change = next(x for x in rows if x['event'] == 'realtime_priority_removed')
        self.assertTrue(any(x.get('demoted') for x in change['changes']))
        self.assertEqual(os.sched_getscheduler(child.pid), os.SCHED_OTHER)
        self.assertIsNone(child.poll())
        child.terminate()
        self.assertEqual(watcher.wait(timeout=5), 0)


if __name__ == '__main__':
    unittest.main()
