"""Exercise acknowledgement and emergency termination against disposable children."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from aeolus_panic_client import PanicRecovery

SERVER = r'''
import socket,signal,sys
if sys.argv[2]=='stuck':signal.signal(signal.SIGTERM,signal.SIG_IGN)
s=socket.socket(socket.AF_UNIX,socket.SOCK_DGRAM);s.bind(sys.argv[1])
epoch=3;ack=3;muted=1;accepted=set()
while True:
 command,client=s.recvfrom(128)
 if command.startswith(b'PANIC '):
  # Delayed stale reply from a preceding STATUS must not acknowledge this panic.
  s.sendto(('STATUS %d %d %d'%(epoch,ack,muted)).encode(),client)
  nonce=command.split()[1];fresh=nonce not in accepted
  if fresh:epoch+=1;accepted.add(nonce)
  if sys.argv[2] in ('ack','drop-first','ack-resumed'):ack=epoch
  if sys.argv[2]=='ack-resumed':muted=0
  if sys.argv[2]=='drop-first' and fresh:continue
 if command==b'RESUME':muted=0
 reply=('STATUS %d %d %d'%(epoch,ack,muted)).encode()
 if command.startswith(b'PANIC '):reply+=b' '+command.split()[1]
 s.sendto(reply,client)
'''


@unittest.skipUnless(sys.platform == 'linux', 'Linux UNIX sockets and pidfds')
class PanicTests(unittest.TestCase):
    def setup_server(self, mode, allow_stop, establish=True):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        path = Path(tmp.name) / 'control.sock'
        child = subprocess.Popen([sys.executable, '-c', SERVER, str(path), mode])
        def cleanup():
            if child.poll() is None: child.kill()
            child.wait(timeout=5)
        self.addCleanup(cleanup)
        deadline = time.monotonic() + 3
        while not path.exists() and time.monotonic() < deadline: time.sleep(.01)
        self.assertTrue(path.exists())
        events = []
        recovery = PanicRecovery(child.pid, Path(sys.executable).resolve(), path,
                                 lambda event, **data: events.append((event, data)),
                                 timeout=.2, kill_after=.15, allow_stop=allow_stop)
        self.addCleanup(recovery.close)
        # Establish the existing epoch before starting a new panic.
        if establish:
            recovery.client.send('STATUS')
            deadline = time.monotonic() + 1
            while recovery.client.requested != 3 and time.monotonic() < deadline:
                recovery.client.poll();time.sleep(.01)
            self.assertEqual(recovery.client.requested, 3)
        return child, recovery, events

    def tick_until(self, recovery, state):
        deadline = time.monotonic() + 3
        while recovery.state != state and time.monotonic() < deadline:
            recovery.tick();time.sleep(.01)
        self.assertEqual(recovery.state, state)

    def test_acknowledged_panic_does_not_stop_application(self):
        child, recovery, events = self.setup_server('ack', True)
        recovery.request()
        self.tick_until(recovery, 'muted')
        self.assertEqual(recovery.client.target, 4)
        self.assertIsNone(child.poll())
        self.assertNotIn('panic_sigterm_sent', [e for e,_ in events])

    def test_stale_acknowledgement_cannot_satisfy_new_panic(self):
        child, recovery, events = self.setup_server('stuck', False)
        recovery.request()
        self.tick_until(recovery, 'failed')
        self.assertEqual(recovery.client.target, 4)
        self.assertIsNone(child.poll())
        self.assertNotIn('panic_acknowledged', [e for e,_ in events])

    def test_unknown_existing_epoch_cannot_make_old_ack_look_current(self):
        child, recovery, events = self.setup_server('ack', True, establish=False)
        recovery.request()
        self.tick_until(recovery, 'muted')
        self.assertEqual(recovery.client.target, 4)
        self.assertIsNone(child.poll())

    def test_lost_correlated_reply_is_retried_without_a_new_panic_epoch(self):
        child, recovery, events = self.setup_server('drop-first', True)
        recovery.request()
        self.tick_until(recovery, 'muted')
        self.assertEqual(recovery.client.target, 4)
        self.assertEqual(recovery.client.requested, 4)
        self.assertIsNone(child.poll())
        self.assertNotIn('panic_sigterm_sent', [e for e,_ in events])

    def test_an_explicit_resume_after_ack_does_not_trigger_forced_exit(self):
        child, recovery, events = self.setup_server('ack-resumed', True)
        recovery.request()
        self.tick_until(recovery, 'idle')
        self.assertEqual(recovery.client.target, 4)
        self.assertTrue(recovery.client.completed())
        self.assertFalse(recovery.client.muted)
        self.assertIsNone(child.poll())
        self.assertIn('panic_acknowledged', [e for e,_ in events])
        self.assertNotIn('panic_sigterm_sent', [e for e,_ in events])

    def test_unresponsive_worker_protocol_escalates_to_exact_child(self):
        child, recovery, events = self.setup_server('stuck', True)
        recovery.request()
        self.tick_until(recovery, 'killed')
        self.assertEqual(child.wait(timeout=3), -9)
        names = [e for e,_ in events]
        self.assertIn('panic_unacknowledged', names)
        self.assertIn('panic_sigterm_sent', names)
        self.assertIn('panic_sigkill_sent', names)


if __name__ == '__main__':
    unittest.main()
