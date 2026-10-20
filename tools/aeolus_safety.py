#!/usr/bin/env python3
# Copyright (C) 2026 Aeolus fork contributors
# SPDX-License-Identifier: GPL-3.0-or-later
"""Independent Linux safety companion for the production standalone.

Uses the same HEALTH 2 / nonce-correlated recovery as the validated VIM4 tests.
No root access, host policy changes, CSV monitoring or automatic app restart.
"""
import argparse
import json
import time
import os
from pathlib import Path
import subprocess
import sys


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--pid',type=int,required=True)
    p.add_argument('--executable',type=Path,required=True)
    p.add_argument('--runtime',type=Path,required=True)
    p.add_argument('--cpus',required=True)
    p.add_argument('--reports',type=Path)
    a=p.parse_args()
    runtime=a.runtime.resolve(strict=True)
    info=runtime.stat()
    if info.st_uid!=os.getuid() or info.st_mode & 0o077:
        p.error('Safety runtime must be a private directory owned by this user')
    if not hasattr(os,'pidfd_open'):
        p.error('Independent safety requires Linux pidfd support and Python 3.9+')
    panel=None
    try:
        # Optional independent GTK button remains available if the organ UI stalls.
        if os.environ.get('AEOLUS_PANIC_PANEL','1')!='0' and os.environ.get('DISPLAY'):
            import importlib.util
            if importlib.util.find_spec('gi') is not None:
                panel=subprocess.Popen([sys.executable,str(Path(__file__).with_name('vim4_panic_panel.py')),
                    '--pid',str(a.pid),'--executable',str(a.executable),'--socket',str(runtime/'control.sock'),
                    '--log',str(runtime/'panel.jsonl'),'--allow-stop','--cpus',a.cpus,'--rt-priority','0'],
                    stdin=subprocess.DEVNULL,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL,
                    start_new_session=True)
        import vim4_test_watchdog
        sys.argv=[str(Path(__file__).with_name('vim4_test_watchdog.py')),
            '--pid',str(a.pid),'--executable',str(a.executable),
            '--timing-file',str(runtime/'unused.csv'),'--log',str(runtime/'watchdog.jsonl'),
            '--panic-socket',str(runtime/'control.sock'),'--cpus',a.cpus,'--allow-stop',
            '--ready-file',str(runtime/'ready'),'--shutdown-file',str(runtime/'closing'),'--wait-for-audio','--detach-file',str(runtime/'detached')]
        return vim4_test_watchdog.main()
    finally:
        if panel is not None:
            try:panel.wait(timeout=1)
            except subprocess.TimeoutExpired:
                panel.terminate()
                try:panel.wait(timeout=1)
                except subprocess.TimeoutExpired:panel.kill();panel.wait()
        # Only our fixed names; never recurse through application/user data.
        target=Path('/proc',str(a.pid),'stat')
        try:exited=target.read_text().rpartition(') ')[2].split()[0]=='Z'
        except FileNotFoundError:exited=True
        if exited or (runtime/'detached').exists():
            preserve=False
            try:
                log=runtime/'watchdog.jsonl'
                events=[json.loads(line) for line in log.read_text().splitlines()]
                if a.reports is not None and any(row.get('event') in
                        ('audio_progress_stalled','application_close_stalled','manual_panic_unacknowledged') for row in events):
                    a.reports.mkdir(parents=True,exist_ok=True)
                    report=a.reports/('safety-'+str(time.time_ns())+'-'+str(a.pid)+'.jsonl')
                    with report.open('x') as out:out.write(log.read_text())
                    report.chmod(0o600)
            except (OSError,ValueError):preserve=True
            for name in ([] if preserve else ['ready','closing','detached','watchdog.jsonl','panel.jsonl','helper.log','control.sock']):
                try:(runtime/name).unlink()
                except FileNotFoundError:pass
            try:runtime.rmdir()
            except OSError:pass

if __name__=='__main__':raise SystemExit(main())
