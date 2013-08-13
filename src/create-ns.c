/* Implements the container interface found in http://www.freedesktop.org/wiki/Software/systemd/ContainerInterface/  */
/* Inspired from clone man page, as well as systemd-nspawn. */
/* Good documentation here: https://lwn.net/Articles/532271/ */
/* gcc create-ns.c -o create-ns -static -Wall -O2 */
/* sudo ./create-ns /usr/local/chroots/xalarm /lib/systemd/systemd */

#define _GNU_SOURCE
#include <sys/stat.h>
#include <sys/prctl.h>
#include <sys/capability.h>
#include <sys/wait.h>
#include <sys/mount.h>
#include <sched.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>

static int verbose = 3;

#define log(level, str, ...) do { \
    if (verbose >= (level)) printf("%s: " str "\n", __func__, ##__VA_ARGS__); \
} while (0)

#define error(str, ...) printf("%s: " str "\n", __func__, ##__VA_ARGS__)

/* Similar to perror, but prints function name as well */
#define syserror(str, ...) do { \
    printf("%s: " str " (%s)\n", \
                    __func__, ##__VA_ARGS__, strerror(errno)); \
    exit(1); \
} while (0)

static int ptfd = -1;

static int mountchroot(const char* root, const char *source, const char *target,
                 const char *filesystemtype, unsigned long mountflags,
                 const void *data) {
    char newtarget[128];
    snprintf(newtarget, 128, "%s%s", root, target);
    log(3, "Mounting %s to %s (%s)", source, newtarget, data);
    return mount(source, newtarget, filesystemtype, mountflags, data);
}

static int devmknod(char* root, char *in, char* out) {
    char oldfn[128];
    char newfn[128];
    struct stat st;
    snprintf(oldfn, 128, "%s", in);
    snprintf(newfn, 128, "%s%s", root, out ? out : in);

    stat(oldfn, &st);

    return mknod(newfn, st.st_mode, st.st_rdev);
}

static int childFunc(void *arg) {
    char **argv = arg;
    char* env[] = {
        "PATH=/usr/sbin:/usr/bin",
        "container=crouton", NULL};

    char* root = argv[0];
    char** init = &argv[1];


    char* name = strdup(ptsname(ptfd));

    log(3, "ptfd=%d|%s\n", ptfd, name);

    close(STDIN_FILENO);
    close(STDOUT_FILENO);
    close(STDERR_FILENO);
    close(ptfd);

    int slavept = open(name, O_RDWR);
    fprintf(stderr, "slavept=%d\n", slavept);
    dup2(STDIN_FILENO, STDOUT_FILENO);
    fprintf(stdout, "stdout\n");
    dup2(STDIN_FILENO, STDERR_FILENO);
    fprintf(stderr, "stderr\n");

    setsid();

    prctl(PR_SET_PDEATHSIG, SIGKILL);

    /* start cgroup */
    // Maybe not needed for now...

    /* Mount needed stuff */
    mountchroot(root, "proc", "/proc", "proc", 0, NULL);
    mountchroot(root, "/proc/sys", "/proc/sys", NULL, MS_BIND, NULL);
    mountchroot(root, NULL, "/proc/sys", NULL, MS_BIND|MS_RDONLY|MS_REMOUNT, NULL);
    mountchroot(root, "sysfs", "/sys", "sysfs", MS_RDONLY, NULL);
    mountchroot(root, "tmpfs", "/dev", "tmpfs", 0, "mode=755");
    char buf[128];
    snprintf(buf, 128, "%s/dev/pts", root);
    mkdir(buf, 0755);
    /* newinstance does not work? */
    mountchroot(root, "devpts", "/dev/pts", "devpts", 0, "newinstance,ptmxmode=0666,mode=620,gid=5");

    devmknod(root, "/dev/null", NULL);
    devmknod(root, "/dev/zero", NULL);
    devmknod(root, "/dev/full", NULL);
    devmknod(root, "/dev/random", NULL);
    devmknod(root, "/dev/urandom", NULL);
    devmknod(root, "/dev/tty", NULL);
    devmknod(root, "/dev/ptmx", NULL);
//    devmknod(root, name, "/dev/console");
/*    snprintf(buf, 128, "%s/dev/ptmx", root);
      symlink("pts/ptmx", buf);*/
    snprintf(buf, 128, "%s/dev/console", root);
    symlink(name, buf);

    if (chroot(root) < 0) {
        syserror("chroot");
    }

    chdir("/");

    /* Drop CAP_MKNOD from children as well */
    prctl(PR_CAPBSET_DROP, CAP_MKNOD);
    prctl(PR_CAPBSET_DROP, CAP_SETPCAP);

    cap_user_header_t hdrp;
    cap_user_data_t datap;

    hdrp = malloc(8);
    datap = malloc(3*4);

    hdrp->pid = 0;
    hdrp->version = _LINUX_CAPABILITY_VERSION;

    if (capget(hdrp, datap) < 0)
        syserror("capget");

    int caps = (1 << CAP_MKNOD) | (1 << CAP_SETPCAP);
    datap->permitted &= ~caps;
    datap->effective &= ~caps;
    datap->inheritable &= ~caps;

    if (capset(hdrp, datap) < 0)
        syserror("capset");

    log(3, "execvpe!");

    execvpe(init[0], init, (char**)env);
    syserror("execvpe");
    exit(1);
}

#define STACK_SIZE (1024 * 1024) /* Stack size for cloned child */

int main(int argc, char** argv) {
    char* stack;
    int pid;
    struct winsize ws;

    if (argc < 3) {
        error("Need 2 args");
        exit(1);
    }

    /* Make a new pt */
    ptfd = posix_openpt(O_RDWR|O_NOCTTY|O_CLOEXEC|O_NDELAY);

    ioctl(STDIN_FILENO, TIOCGWINSZ, &ws);
    ioctl(ptfd, TIOCGWINSZ, &ws);

    unlockpt(ptfd);

    stack = malloc(STACK_SIZE);
    if (!stack)
        syserror("malloc");

    int fl = CLONE_NEWPID | CLONE_NEWNS | SIGCHLD;
    fl |= CLONE_NEWIPC | CLONE_NEWUTS;

    pid = clone(childFunc, stack+STACK_SIZE,
                fl, &argv[1]);
    if (pid == -1)
        syserror("clone");
    log(3, "clone() returned %ld\n", (long) pid);

    /* Add non-blocking flag */
    int flags = fcntl(ptfd, F_GETFL, 0);
    if (flags < 0 || fcntl(ptfd, F_SETFL, flags | O_NONBLOCK) < 0) {
        syserror("error in fnctl GETFL/SETFL.");
    }

    flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    if (flags < 0 || fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK) < 0) {
        syserror("error in fnctl GETFL/SETFL.");
    }

    char buffer[1024];

    pid_t wait_pid;

    while (1) {
        wait_pid = waitpid(pid, NULL, WNOHANG);

        int n = read(ptfd, buffer, 1024);
        if (n > 0) {
            log(4, "read fd=%d n=%d", ptfd, n);
            buffer[n] = 0;
            printf("%s", buffer);
        }

        n = read(STDIN_FILENO, buffer, 1024);
        if (n > 0) {
            log(4, "read fd=0 n=%d", n);
            write(ptfd, buffer, n);
        }

        if (wait_pid == -1) {
            error("waitpid error.");
            break;
        } else if (wait_pid == pid) {
            log(3, "child exited!");
            break;
        }
    }

    log(3, "child has terminated\n");
    return 0;
}
