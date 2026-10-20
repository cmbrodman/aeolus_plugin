"""Use disposable children and a fake debugger; never attach to a real app."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

TOOL = Path(__file__).resolve().parents[1]/'tools/claude_panic/capture_hang.py'


@unittest.skipUnless(sys.platform == 'linux', 'Linux procfs observer')
class CaptureTests(unittest.TestCase):
    def run_case(self, *, wrong=False, timeout=False, immediate=False):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            timing = root/'timing.csv'
            child = subprocess.Popen([sys.executable, '-c',
                'import sys,time\nfrom pathlib import Path\np=Path(sys.argv[1])\n'
                'for n in range(10):\n p.write_text("x"*(n+1));time.sleep(.05)\n'
                'time.sleep(10)', str(timing)])
            try:
                debugger = root/'gdb'
                debugger.write_text('#!'+sys.executable+'\nimport os,signal,sys,time\nprint("fake stack",flush=True)\n'+
                                    ('pid=int(next(x for x in sys.argv if x.startswith("attach ")).split()[1]);os.kill(pid,signal.SIGSTOP);time.sleep(10)\n' if timeout else ''))
                debugger.chmod(0o700)
                env = os.environ.copy();env['PATH'] = str(root)+os.pathsep+env['PATH']
                output = root/'capture'
                command = [sys.executable,str(TOOL),'--pid',str(child.pid),
                           '--executable',str(Path('/bin/false') if wrong else Path(sys.executable).resolve()),
                           '--timing-file',str(timing),'--output-directory',str(output),
                           '--stall-seconds','.25','--gdb-timeout','.2','--duration','1.5']
                if immediate:command.append('--capture-now')
                result = subprocess.run(command, env=env, capture_output=True, text=True, timeout=5)
                self.assertIsNone(child.poll(), 'Observer must never kill its target')
                state = Path('/proc',str(child.pid),'stat').read_text().rpartition(') ')[2].split()[0]
                self.assertNotIn(state, ('T','t'), 'Debugger timeout must not leave target stopped')
                if wrong:
                    self.assertNotEqual(result.returncode,0)
                    self.assertFalse(output.exists())
                else:
                    self.assertEqual(result.returncode,0,result.stderr)
                    rows = [json.loads(line) for line in (output/'samples.jsonl').read_text().splitlines()]
                    captures = [row for row in rows if row.get('event')=='stack_capture']
                    self.assertEqual(len(captures),1)
                    self.assertEqual(captures[0]['result'],'gdb_timeout' if timeout else 0)
                    self.assertIn('fake stack',(output/'stacks.txt').read_text())
            finally:
                child.terminate();child.wait(timeout=5)

    def test_stall_captures_once_without_stopping_target(self):
        self.run_case()

    def test_bounded_debugger_timeout_keeps_target_alive(self):
        self.run_case(timeout=True,immediate=True)

    def test_wrong_executable_rejected(self):
        self.run_case(wrong=True)


if __name__ == '__main__':
    unittest.main()
