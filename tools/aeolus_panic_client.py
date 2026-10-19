#!/usr/bin/env python3
"""Independent control for one explicitly identified, test-owned Aeolus process."""
import os
from pathlib import Path
import secrets
import signal
import socket
import time


class PanicClient:
    def __init__(self, path):
        self.path = str(path)
        self.socket = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
        self.socket.setblocking(False)
        self.socket.bind('\0aeolus-panic-' + str(os.getpid()) + '-' + secrets.token_hex(8))
        self.requested = self.acknowledged = 0
        self.muted = False
        self.resuming = False
        self.target = None
        self.minimum_epoch = 0
        self.nonce = None
        self.error = None
        self.audio_started = self.audio_completed = self.gui_progress = None
        self.audio_active = False
        self.audio_state = None
        self.health_received_at = None

    def close(self):
        self.socket.close()

    def send(self, command):
        try:
            self.socket.sendto(command.encode('ascii'), self.path)
            self.error = None
            return True
        except OSError as error:
            self.error = str(error)
            return False

    def panic(self):
        self.target = None
        # Clear stale STATUS replies so an earlier acknowledgement cannot satisfy
        # this new request. Datagram ordering is preserved for this sender.
        self.poll()
        self.target = None
        self.minimum_epoch = self.requested + 1
        self.nonce = secrets.token_hex(8)
        self.send('PANIC ' + self.nonce)

    def poll(self):
        while True:
            try:
                packet = self.socket.recv(256).decode('ascii').split()
            except BlockingIOError:
                break
            except (OSError, UnicodeError):
                break
            if len(packet) == 6 and packet[:2] == ['HEALTH', '2']:
                try:
                    started, completed, active, gui = map(int, packet[2:])
                except ValueError:
                    continue
                if (min(started, completed, gui) < 0 or max(started, completed, gui) >= 2**64
                        or completed > started or active not in (0, 1, 2)):
                    continue
                if self.audio_completed is not None and completed < self.audio_completed:
                    continue
                self.audio_started, self.audio_completed = started, completed
                self.audio_state, self.audio_active, self.gui_progress = active, active == 1, gui
                self.health_received_at = time.monotonic()
                continue
            if len(packet) not in (4, 5) or packet[0] != 'STATUS':
                continue
            try:
                requested, acknowledged, muted = map(int, packet[1:4])
            except ValueError:
                continue
            if requested < 0 or requested >= 2**32 or acknowledged < 0 or acknowledged > requested or muted not in (0, 1, 2):
                continue
            self.requested, self.acknowledged, self.muted = requested, acknowledged, bool(muted)
            self.resuming = muted == 2
            correlated = self.nonce is None or (len(packet) == 5 and packet[4] == self.nonce)
            if self.target is None and correlated and requested >= self.minimum_epoch:
                self.target = requested

    def completed(self):
        # A correlated acknowledgement proves the panic completed. Another
        # controller may already have explicitly resumed audio; do not kill it.
        return self.target is not None and self.target > 0 and self.acknowledged >= self.target


class PanicRecovery:
    def __init__(self, pid, executable, path, emit, timeout=1.0, kill_after=3.0, allow_stop=False):
        from vim4_test_watchdog import identity
        self.pid = pid
        if pid <= 1 or pid == os.getpid():
            raise ValueError('Specify the isolated application process')
        self.expected = Path(executable).resolve(strict=True)
        self.original = identity(pid)
        if self.original[0] != os.getuid() or self.original[2] == 'Z' or self.original[3] != self.expected:
            raise ValueError('Panic target must be a live, owned process with the exact executable')
        self.pidfd = os.pidfd_open(pid)
        self.client = PanicClient(path)
        self.emit = emit
        self.timeout, self.kill_after, self.allow_stop = timeout, kill_after, allow_stop
        self.state = 'idle'
        self.deadline = 0
        self.correlation_retry = False

    def alive(self):
        from vim4_test_watchdog import identity
        try:
            current = identity(self.pid)
            return current[:2] == self.original[:2] and current[2] != 'Z' and current[3] == self.expected
        except (FileNotFoundError, ProcessLookupError):
            return False

    def close(self):
        self.client.close()
        os.close(self.pidfd)

    def request(self):
        if self.state in ('pending', 'terminating', 'killed'):
            return
        self.client.panic()
        self.state = 'pending'
        self.correlation_retry = False
        self.deadline = time.monotonic() + self.timeout
        self.emit('panic_requested', timeout_seconds=self.timeout)

    def tick(self):
        if not self.alive():
            return False
        self.client.poll()
        self.client.send('STATUS')
        now = time.monotonic()
        if self.state == 'pending':
            if self.client.completed():
                self.state = 'muted' if self.client.muted else 'idle'
                self.emit('panic_acknowledged', epoch=self.client.target, muted=self.client.muted)
            elif now >= self.deadline:
                if self.client.target is None and not self.correlation_retry:
                    # A missing datagram must not turn a healthy acknowledgement
                    # into a forced exit. The receiver deduplicates this nonce.
                    self.correlation_retry = True
                    self.client.send('PANIC ' + self.client.nonce)
                    self.deadline = now + self.timeout
                    self.emit('panic_correlation_retry', timeout_seconds=self.timeout)
                    return True
                self.emit('panic_unacknowledged', socket_error=self.client.error)
                if self.allow_stop:
                    # pidfd identifies the original process even if its PID is reused.
                    try:
                        signal.pidfd_send_signal(self.pidfd, signal.SIGTERM)
                    except ProcessLookupError:
                        return False
                    self.emit('panic_sigterm_sent')
                    self.state = 'terminating'
                    self.deadline = now + self.kill_after
                else:
                    self.state = 'failed'
            else:
                if self.client.target is None:
                    self.client.send('PANIC ' + self.client.nonce)
                self.client.send('STATUS')
        elif self.state == 'terminating' and now >= self.deadline:
            try:
                signal.pidfd_send_signal(self.pidfd, signal.SIGKILL)
            except ProcessLookupError:
                return False
            self.emit('panic_sigkill_sent')
            self.state = 'killed'
        return True
