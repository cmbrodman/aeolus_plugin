#!/usr/bin/env python3
"""Exercise the read-only observer against a real Unix datagram endpoint."""
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import threading
import unittest

class CaptureMidiTests(unittest.TestCase):
    @unittest.skipUnless(hasattr(os, 'pidfd_open') and Path('/dev/shm').is_dir(), 'Linux RAM/pidfd test')
    def test_counters_keys_and_read_only_commands(self):
        with tempfile.TemporaryDirectory(prefix='aeolus-midi-test-',dir='/dev/shm') as folder:
            root=Path(folder);endpoint=root/'control.sock'
            server=socket.socket(socket.AF_UNIX,socket.SOCK_DGRAM);server.bind(str(endpoint));server.settimeout(.1)
            stop=threading.Event();commands=[]
            def run():
                while not stop.is_set():
                    try:data,address=server.recvfrom(256)
                    except socket.timeout:continue
                    cmd=data.decode();commands.append(cmd)
                    if cmd=='DIAG':reply='DIAG 1 '+' '.join(map(str,range(24)))
                    elif cmd=='HEALTH':reply='HEALTH 2 100 100 1 12'
                    elif cmd=='STATUS':reply='STATUS 0 0 0'
                    else:
                        _,ch,note=cmd.split()
                        reply=f'NOTE 1 {ch} {note} '+' '.join(['1']*10)
                        # An unrelated late packet must not corrupt per-key attribution.
                        server.sendto(b'STATUS 0 0 0',address)
                    try:server.sendto(reply.encode(),address)
                    except OSError:break
            worker=threading.Thread(target=run);worker.start()
            try:
                tool=Path(__file__).resolve().parents[1]/'tools/claude_panic/capture_midi.py'
                subprocess.run([sys.executable,str(tool),'--pid',str(os.getpid()),
                    '--executable',os.readlink('/proc/self/exe'),'--socket',str(endpoint),
                    '--output',str(root/'capture'),'--duration','.6'],check=True,timeout=10)
            finally:
                stop.set();worker.join(timeout=2);server.close()
            rows=[json.loads(line) for line in (root/'capture/midi.jsonl').read_text().splitlines()]
            summaries=[row for row in rows if 'stages' in row]
            self.assertTrue(summaries)
            self.assertEqual(summaries[0]['stages']['engine'],{'on':8,'off':9})
            self.assertEqual(summaries[0]['extra']['reset_pruned_off'],23)
            self.assertTrue(any(row.get('key',[])[:4]==['NOTE','1','1','60'] for row in rows))
            self.assertTrue(all(cmd.split()[0] in {'DIAG','HEALTH','STATUS','NOTE'} for cmd in commands))

if __name__=='__main__':unittest.main()
