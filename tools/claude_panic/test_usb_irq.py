#!/usr/bin/env python3
"""Temporary VIM4 USB IRQ placement experiment; restores affinity on exit."""
import argparse
import json
import os
from pathlib import Path
import select
import signal
import time


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--pid',type=int,required=True)
    p.add_argument('--executable',type=Path,required=True)
    p.add_argument('--irq',type=int,default=37)
    p.add_argument('--cpu',type=int,default=4)
    p.add_argument('--seconds',type=int,default=900)
    p.add_argument('--check',action='store_true',help='Read-only preflight; no root needed')
    a=p.parse_args()
    if a.pid<=1 or a.cpu not in (4,5,6,7) or not 1<=a.seconds<=1800:
        p.error('Expected an owned test PID, efficiency CPU 4–7 and duration 1–1800 seconds')
    expected=str(a.executable.resolve(strict=True))
    proc=Path('/proc',str(a.pid))
    if os.readlink(proc/'exe')!=expected:
        p.error('Process does not match the exact test executable')
    target=Path('/proc/irq',str(a.irq),'smp_affinity_list')
    effective=target.with_name('effective_affinity_list')
    lines=[s for s in Path('/proc/interrupts').read_text().splitlines()
           if s.split(':',1)[0].strip()==str(a.irq)]
    if len(lines)!=1 or 'xhci' not in lines[0].lower():
        p.error('Selected IRQ is not the observed xHCI USB controller')
    original=target.read_text().strip()
    print(json.dumps({'event':'preflight','pid':a.pid,'irq':a.irq,'cpu':a.cpu,
        'original_allowed':original,'original_effective':effective.read_text().strip(),
        'duration_seconds':a.seconds}),flush=True)
    if a.check:return
    if os.geteuid()!=0:p.error('Run with sudo to change this IRQ affinity')
    fd=os.pidfd_open(a.pid)
    stopping=False
    def stop(signum,frame):
        nonlocal stopping
        stopping=True
    for sig in (signal.SIGINT,signal.SIGTERM,signal.SIGHUP):signal.signal(sig,stop)
    try:
        if os.readlink(proc/'exe')!=expected or select.select([fd],[],[],0)[0]:
            raise RuntimeError('Target exited or changed before the experiment')
        target.write_text(str(a.cpu)+'\n')
        current=effective.read_text().strip()
        if current!=str(a.cpu):raise RuntimeError('IRQ effective affinity did not move: '+current)
        print(json.dumps({'event':'irq_moved','effective':current,
            'time_ns':time.monotonic_ns(),'restore_allowed':original}),flush=True)
        deadline=time.monotonic()+a.seconds
        while not stopping and time.monotonic()<deadline:
            if select.select([fd],[],[],min(.25,max(0,deadline-time.monotonic())))[0]:break
    finally:
        try:
            target.write_text(original+'\n')
            print(json.dumps({'event':'irq_restored','allowed':target.read_text().strip(),
                'effective':effective.read_text().strip(),'time_ns':time.monotonic_ns()}),flush=True)
        finally:os.close(fd)

if __name__=='__main__':main()
