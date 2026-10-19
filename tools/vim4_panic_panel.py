#!/usr/bin/env python3
"""Separate VIM4 panic panel; its event loop never runs inside Aeolus."""
import argparse
import json
import os
from pathlib import Path
import time
from aeolus_panic_client import PanicRecovery


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--pid', type=int, required=True)
    parser.add_argument('--executable', type=Path, required=True)
    parser.add_argument('--socket', type=Path, required=True)
    parser.add_argument('--log', type=Path, required=True)
    parser.add_argument('--allow-stop', action='store_true')
    parser.add_argument('--cpus', default='5')
    parser.add_argument('--rt-priority', type=int, default=1)
    args = parser.parse_args()
    os.sched_setaffinity(0, {int(v) for v in args.cpus.split(',')})
    if args.rt_priority:
        os.sched_setscheduler(0, os.SCHED_FIFO, os.sched_param(args.rt_priority))
    import gi
    gi.require_version('Gtk', '3.0')
    from gi.repository import Gtk, GLib
    with args.log.open('x') as log:
        def emit(event, **data):
            log.write(json.dumps({'event': event, 'time_ns': time.monotonic_ns(), **data}) + '\n')
            log.flush()
        recovery = PanicRecovery(args.pid, args.executable, args.socket, emit, allow_stop=args.allow_stop)
        window = Gtk.Window(title='Aeolus Safety Control')
        window.set_keep_above(True)
        window.set_default_size(360, 210)
        box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=12)
        box.set_border_width(16)
        window.add(box)
        status = Gtk.Label(label='Ready')
        status.set_line_wrap(True)
        panic = Gtk.Button(label='PANIC — SILENCE AUDIO')
        panic.set_size_request(320, 80)
        panic.get_style_context().add_class('destructive-action')
        resume = Gtk.Button(label='Resume audio')
        resume.set_sensitive(False)
        box.pack_start(status, False, False, 0)
        box.pack_start(panic, True, True, 0)
        box.pack_start(resume, False, False, 0)
        panic.connect('clicked', lambda *_: recovery.request())
        def restart_audio(*_):
            recovery.client.send('RESUME')
            status.set_text('Resuming audio…')
        resume.connect('clicked', restart_audio)
        def tick():
            if not recovery.tick():
                Gtk.main_quit()
                return False
            recovery.client.send('STATUS')
            if recovery.state == 'pending': status.set_text('Silencing audio; waiting for workers…')
            elif recovery.state == 'terminating': status.set_text('Workers did not respond; stopping Aeolus…')
            elif recovery.state == 'failed': status.set_text('Panic failed; emergency stop is disabled.')
            elif recovery.client.resuming: status.set_text('Resuming audio…')
            elif recovery.client.muted: status.set_text('Audio silenced. Resume when ready.')
            else: status.set_text('Ready')
            panic.set_sensitive(recovery.state not in ('pending', 'terminating', 'killed'))
            resume.set_sensitive(recovery.client.muted and not recovery.client.resuming
                                 and recovery.client.requested == recovery.client.acknowledged)
            return True
        window.connect('destroy', lambda *_: Gtk.main_quit())
        GLib.timeout_add(50, tick)
        window.show_all()
        panic.grab_focus()
        emit('panel_started', pid=os.getpid(), cpus=sorted(os.sched_getaffinity(0)),
             scheduler=os.sched_getscheduler(0), priority=os.sched_getparam(0).sched_priority)
        try:
            Gtk.main()
        finally:
            recovery.close()
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
