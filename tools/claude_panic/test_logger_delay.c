// Copyright (C) 2026 Aeolus fork contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Linux hardware fault injection, explicitly LD_PRELOADed into an isolated test.
// Delay only the first write by aeolus-log; no host or normal app configuration.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

ssize_t write(int fd, const void* bytes, size_t count)
{
    static atomic_int delayed = 0;
    const char* delay = getenv("AEOLUS_TEST_LOGGER_DELAY_MS");
    if (delay != NULL) {
        char name[16] = {0};
        pthread_getname_np(pthread_self(), name, sizeof(name));
        int expected = 0;
        if (strcmp(name, "aeolus-log") == 0 &&
            atomic_compare_exchange_strong(&delayed, &expected, 1)) {
            long milliseconds = strtol(delay, NULL, 10);
            if (milliseconds > 0 && milliseconds <= 30000) {
                const char* marker = getenv("AEOLUS_TEST_LOGGER_DELAY_MARKER");
                if (marker != NULL) {
                    int out = open(marker, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
                    if (out >= 0) {
                        struct timespec now;
                        clock_gettime(CLOCK_MONOTONIC, &now);
                        char message[64];
                        int size = snprintf(message, sizeof(message), "%lld\n",
                            (long long) now.tv_sec * 1000000000LL + now.tv_nsec);
                        syscall(SYS_write, out, message, (size_t) size);
                        close(out);
                    }
                }
                struct timespec pause = {milliseconds / 1000, (milliseconds % 1000) * 1000000};
                while (nanosleep(&pause, &pause) != 0 && errno == EINTR) {}
            }
        }
    }
    return syscall(SYS_write, fd, bytes, count);
}
