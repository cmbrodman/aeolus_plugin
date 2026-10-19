# Copyright (C) 2026 Aeolus fork contributors
# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise production companion startup, recovery and runtime cleanup."""
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time
import unittest
from test_audio_health_watchdog import SERVER

COMPANION=Path(__file__).resolve().parents[1]/'tools/aeolus_safety.py'

@unittest.skipUnless(sys.platform=='linux' and hasattr(os,'pidfd_open') and Path('/dev/shm').is_dir(),'Linux pidfd/RAM checks')
class ProductionSafetyTests(unittest.TestCase):
    def launch(self, reports=False):
        tmp=tempfile.TemporaryDirectory(prefix='aeolus-prod-test-',dir='/dev/shm');self.addCleanup(tmp.cleanup)
        root=Path(tmp.name);runtime=root/'runtime';runtime.mkdir(mode=0o700)
        app=subprocess.Popen([sys.executable,'-c',SERVER,str(runtime/'control.sock'),str(root/'unused.csv'),'csv-stuck'])
        self.addCleanup(self.stop,app)
        deadline=time.monotonic()+3
        while not (runtime/'control.sock').exists() and time.monotonic()<deadline:time.sleep(.01)
        env=os.environ.copy();env['AEOLUS_PANIC_PANEL']='0'
        guard=subprocess.Popen([sys.executable,str(COMPANION),'--pid',str(app.pid),
            '--executable',str(Path(sys.executable).resolve()),'--runtime',str(runtime),
            '--cpus',','.join(map(str,os.sched_getaffinity(0))),
            *(['--reports',str(root/'reports')] if reports else [])],env=env,
            stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
        self.addCleanup(self.stop,guard)
        deadline=time.monotonic()+3
        while not (runtime/'ready').exists() and time.monotonic()<deadline:time.sleep(.01)
        self.assertTrue((runtime/'ready').exists(),'production watchdog failed to arm')
        return app,guard,runtime

    @staticmethod
    def stop(process):
        if process.poll() is None:process.kill()
        process.wait(timeout=3)

    def test_starts_without_csv_and_cleans_private_runtime_on_exit(self):
        app,guard,runtime=self.launch()
        rows=[json.loads(s) for s in (runtime/'watchdog.jsonl').read_text().splitlines()]
        self.assertFalse(rows[0]['csv_monitoring'])
        self.assertEqual(rows[0]['priority'],0)
        app.terminate();app.wait(timeout=3);guard.wait(timeout=3)
        self.assertEqual(guard.returncode,0)
        self.assertFalse(runtime.exists())

    def test_frozen_app_is_terminated_without_restart(self):
        app,guard,runtime=self.launch(reports=True)
        os.kill(app.pid,signal.SIGSTOP)
        app.wait(timeout=15);guard.wait(timeout=3)
        self.assertEqual(app.returncode,-signal.SIGKILL)
        self.assertEqual(guard.returncode,0)
        self.assertFalse(runtime.exists())
        reports=list((runtime.parent/'reports').glob('safety-*.jsonl'))
        self.assertEqual(len(reports),1)
        events=[json.loads(line)['event'] for line in reports[0].read_text().splitlines()]
        self.assertIn('panic_sigkill_sent',events)
        self.assertEqual(reports[0].stat().st_mode & 0o777,0o600)

    def test_processor_detach_leaves_the_process_running(self):
        app,guard,runtime=self.launch()
        (runtime/'closing').touch()
        time.sleep(.15)
        (runtime/'detached').touch()
        guard.wait(timeout=3)
        self.assertEqual(guard.returncode,0)
        self.assertIsNone(app.poll())
        self.assertFalse(runtime.exists())

    def test_rejects_nonprivate_runtime(self):
        with tempfile.TemporaryDirectory(dir='/dev/shm') as tmp:
            path=Path(tmp);path.chmod(0o755)
            result=subprocess.run([sys.executable,str(COMPANION),'--pid',str(os.getpid()),
                '--executable',str(Path(sys.executable).resolve()),'--runtime',tmp,
                '--cpus',str(min(os.sched_getaffinity(0)))],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
            self.assertNotEqual(result.returncode,0)

if __name__=='__main__':unittest.main()
