/* Copyright (c) 2013 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 *
 * Implements the container interface found at
 * http://www.freedesktop.org/wiki/Software/systemd/ContainerInterface/
 * Inspired from clone man page, as well as systemd-nspawn.
 * Good documentation here: https://lwn.net/Articles/532271/
 *
 * gcc create-ns.c -o create-ns -static -Wall -O2
 */

#define _GNU_SOURCE
#include <sys/stat.h>
#include <sys/prctl.h>
#include <sys/capability.h>
#include <sys/wait.h>
#include <sys/ioctl.h>
#include <sched.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <termios.h>
#include <poll.h>

static int verbose = 0;

#define log(level, str, ...) do { \
    if (verbose >= (level)) printf("%s: " str "\r\n", __func__, ##__VA_ARGS__); \
} while (0)

#define error(str, ...) printf("%s: " str "\r\n", __func__, ##__VA_ARGS__)

/* Similar to perror, but prints function name as well */
#define syserror(str, ...) \
    printf("%s: " str " (%s)\r\n", \
                    __func__, ##__VA_ARGS__, strerror(errno));

static int ptfd = -1;

static int childFunc(void *arg) {
    char** init = arg;
    char* name = strdup(ptsname(ptfd));

    log(3, "ptfd=%d|%s", ptfd, name);

    close(STDIN_FILENO);
    close(STDOUT_FILENO);
    close(STDERR_FILENO);
    close(ptfd);

    int slavept = open(name, O_RDWR);
    if (slavept != STDIN_FILENO) {
        syserror("slavept (%d) != 0", slavept);
        return 1;
    }
    if (dup2(STDIN_FILENO, STDOUT_FILENO) < 0 ||
        dup2(STDIN_FILENO, STDERR_FILENO) < 0) {
        syserror("dup2");
        return 1;
    }

    if (setsid() < 0) {
        syserror("setsid");
        return 1;
    }

    if (prctl(PR_SET_PDEATHSIG, SIGKILL) < 0) {
        syserror("prctl");
        return 1;
    }

    /* start cgroup */
    // Maybe not needed for now...

    /* We can do without dropping those caps... */
#if 0
    /* Drop CAP_MKNOD from children as well */
    prctl(PR_CAPBSET_DROP, CAP_MKNOD);
    prctl(PR_CAPBSET_DROP, CAP_SETPCAP);

    cap_user_header_t hdrp;
    cap_user_data_t datap;

    hdrp = malloc(8);
    datap = malloc(3*4*2);

    hdrp->pid = 0;
    hdrp->version = _LINUX_CAPABILITY_VERSION_2;

    if (capget(hdrp, datap) < 0) {
        syserror("capget");
        return 1;
    }

    int caps = (1 << CAP_MKNOD) | (1 << CAP_SETPCAP);
    datap->permitted &= ~caps;
    datap->effective &= ~caps;
    datap->inheritable &= ~caps;

    if (capset(hdrp, datap) < 0) {
        syserror("capset");
        return 1;
    }
#endif

    log(3, "execvpe!");

    execvp(init[0], init);
    syserror("execvpe");
    return 1;
}

#define STACK_SIZE (1024 * 1024) /* Stack size for cloned child */

int main(int argc, char** argv) {
    char* stack;
    int pid;
    struct winsize ws;
    int termios_saved_valid = 0;
    struct termios termios_saved, termios_raw;

    if (argc < 2) {
        error("Need at least 1 arg.");
        exit(1);
    }

    if (tcgetattr(STDIN_FILENO, &termios_saved) >= 0) {
        termios_saved_valid = 1;

        termios_raw = termios_saved;
        cfmakeraw(&termios_raw);
        termios_raw.c_lflag &= ~ECHO;
        tcsetattr(STDIN_FILENO, TCSANOW, &termios_raw);
    }

    /* Make a new pt */
    ptfd = posix_openpt(O_RDWR|O_NOCTTY|O_CLOEXEC|O_NDELAY);

    if (ptfd < 0) {
        syserror("posix_openpt");
        goto exit;
    }

    ioctl(STDIN_FILENO, TIOCGWINSZ, &ws);
    ioctl(ptfd, TIOCGWINSZ, &ws);

    if (unlockpt(ptfd) < 0) {
        syserror("unlockpt");
        goto exit;
    }

    stack = malloc(STACK_SIZE);
    if (!stack) {
        syserror("malloc");
        goto exit;
    }

    int fl = CLONE_NEWPID | CLONE_NEWNS | SIGCHLD;
    fl |= CLONE_NEWIPC | CLONE_NEWUTS;

    pid = clone(childFunc, stack+STACK_SIZE,
                fl, &argv[1]);
    if (pid == -1) {
        syserror("clone");
        goto exit;
    }
    log(3, "clone() returned %ld", (long) pid);

    /* Add non-blocking flag */
    int flags = fcntl(ptfd, F_GETFL, 0);
    if (flags < 0 || fcntl(ptfd, F_SETFL, flags | O_NONBLOCK) < 0) {
        syserror("error in fnctl GETFL/SETFL.");
        goto exit;
    }

    flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    if (flags < 0 || fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK) < 0) {
        syserror("error in fnctl GETFL/SETFL.");
        goto exit;
    }

    char buffer[1024];
    pid_t wait_pid;

    struct pollfd fds[2];
    fds[0].events = POLLIN;
    fds[0].fd = STDIN_FILENO;
    fds[1].events = POLLIN;
    fds[1].fd = ptfd;

    /* FIXME: Use SIGCHLD instead of timeout */
    while (1) {
        wait_pid = waitpid(pid, NULL, WNOHANG);

        int polln = poll(fds, 2, (waitpid == pid) ? 0 : 100);

        if (polln < 0) {
            syserror("poll");
            goto exit;
        }

        if (fds[0].revents & POLLIN) {
            int n = read(STDIN_FILENO, buffer, 1024);
            if (n > 0) {
                log(4, "read fd=0 n=%d", n);
                write(ptfd, buffer, n);
            }
        }
        if (fds[1].revents & POLLIN) {
            int n = read(ptfd, buffer, 1024);
            if (n > 0) {
                log(4, "read fd=%d n=%d", ptfd, n);
                write(STDOUT_FILENO, buffer, n);
            }
        }

        if (wait_pid == -1) {
            error("waitpid error.");
            break;
        } else if (wait_pid == pid) {
            log(3, "child exited!");
            break;
        }
    }

    log(3, "child has terminated");

exit:
    if (termios_saved_valid) {
        tcsetattr(STDIN_FILENO, TCSANOW, &termios_saved);
    }
    /* FIXME: Return child status */
    return 0;
}
