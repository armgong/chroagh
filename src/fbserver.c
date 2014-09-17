
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>
#include <X11/extensions/XTest.h>
#include <assert.h>
#include <poll.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/tcp.h>

const int PORT = 30002;
const int BUFFERSIZE = 4096;

/* 0 - Quiet
 * 1 - General messages (init, new connections)
 * 2 - 1 + Information on each transfer
 * 3 - 2 + Extra information */
static int verbose = 0;

static int server_fd = -1;

#define log(level, str, ...) do { \
    if (verbose >= (level)) printf("%s: " str "\n", __func__, ##__VA_ARGS__); \
} while (0)

#define error(str, ...) printf("%s: " str "\n", __func__, ##__VA_ARGS__)

/* Similar to perror, but prints function name as well */
#define syserror(str, ...) printf("%s: " str " (%s)\n", \
                    __func__, ##__VA_ARGS__, strerror(errno))

/* Run external command, piping some data on its stdin, and reading back
 * the output. Returns the number of bytes read from the process (at most
 * outlen), or -1 on error. */
static int popen2(char* cmd, char* input, int inlen, char* output, int outlen,
                  char *const argv[]) {
    pid_t pid = 0;
    int stdin_fd[2];
    int stdout_fd[2];
    int ret = -1;

    if (pipe(stdin_fd) < 0 || pipe(stdout_fd) < 0) {
        syserror("Failed to create pipe.");
        return -1;
    }

    pid = fork();

    if (pid < 0) {
        syserror("Fork error.");
        return -1;
    } else if (pid == 0) {
        /* Child: connect stdin/out to the pipes, close the unneeded halves */
        close(stdin_fd[1]);
        dup2(stdin_fd[0], STDIN_FILENO);
        close(stdout_fd[0]);
        dup2(stdout_fd[1], STDOUT_FILENO);

        if (argv) {
            execvp(cmd, argv);
        } else {
            execlp(cmd, cmd, NULL);
        }

        error("Error running '%s'.", cmd);
        exit(1);
    }

    /* Parent */

    /* Write input, and read output, while waiting for process termination.
     * This could be done without polling, by reacting on SIGCHLD, but this is
     * good enough for our purpose, and slightly simpler. */
    struct pollfd fds[2];
    fds[0].events = POLLIN;
    fds[0].fd = stdout_fd[0];
    fds[1].events = POLLOUT;
    fds[1].fd = stdin_fd[1];

    pid_t wait_pid;
    int readlen = 0;
    int writelen = 0;
    while (1) {
        /* Get child status */
        wait_pid = waitpid(pid, NULL, WNOHANG);
        /* Check if there is data to read, no matter the process status. */
        /* Timeout after 10ms, or immediately if the process exited already */
        int polln = poll(fds, 2, (wait_pid == pid) ? 0 : 10);

        if (polln < 0) {
            syserror("poll error.");
            goto error;
        }

        log(3, "poll=%d (%d)", polln, (wait_pid == pid));

        /* We can write something to stdin */
        if (fds[1].revents & POLLOUT) {
            if (inlen > writelen) {
                int n = write(stdin_fd[1], input+writelen, inlen-writelen);
                if (n < 0) {
                    error("write error.");
                    goto error;
                }
                log(3, "write n=%d/%d", n, inlen);
                writelen += n;
            }

            if (writelen == inlen) {
                /* Done writing: Only poll stdout from now on. */
                close(stdin_fd[1]);
                stdin_fd[1] = -1;
                fds[1].fd = -1;
            }
            polln--;
        }

        /* We can read something from stdout */
        if (fds[0].revents & POLLIN) {
            int n = read(stdout_fd[0], output+readlen, outlen-readlen);
            if (n < 0) {
                error("read error.");
                goto error;
            }
            log(3, "read n=%d", n);

            readlen += n;
            if (readlen >= outlen) {
                error("Output too long.");
                ret = readlen;
                goto error;
            }
            polln--;
        }

        if (polln != 0) {
            error("Unknown poll event (%d).", fds[0].revents);
            goto error;
        }

        if (wait_pid == -1) {
            error("waitpid error.");
            goto error;
        } else if (wait_pid == pid) {
            log(3, "child exited!");
            break;
        }
    }

    if (stdin_fd[1] >= 0)
        close(stdin_fd[1]);
    close(stdout_fd[0]);
    return readlen;

error:
    if (stdin_fd[1] >= 0)
        close(stdin_fd[1]);
    /* Closing the stdout pipe forces the child process to exit */
    close(stdout_fd[0]);
    /* Try to wait 10ms for the process to exit, then bail out. */
    waitpid(pid, NULL, 10);
    return ret;
}

static Display *dpy;

int init_display() {
    dpy = XOpenDisplay(NULL);

    /* Fixme: check XTest extension available */

    return 0;
}

struct cache_entry {
    uint64_t paddr;
    char file[256];
};

struct cache_entry cache[2];
int next_entry;

// write_image(newclient_fd, width, height, shm, paddr, sig);
int write_image(int fd, int width, int height,
                int shm, uint64_t paddr, uint64_t sig) {
    /* TODO: Resize display as necessary */

    Window root = DefaultRootWindow(dpy);
    XImage *img = XGetImage(dpy, root, 0, 0, width, height, AllPlanes, ZPixmap);
    //printf("size %d %d\n", img->bytes_per_line, img->height);

    int size = img->bytes_per_line * img->height;

/*    uint32_t* ptr = (uint32_t*)img->data;
    int i;
    for (i = 0; i < size/4; i++) {
        ptr[i] = (ptr[i] & 0x000000ff) << 16 |
                 (ptr[i] & 0x0000ff00) |
                 (ptr[i] & 0x00ff0000) >> 16 |
                 0xff000000;
        //ptr[i] = 0xff0000ff;
        }*/

    if (shm) {
        struct cache_entry* entry = NULL;

        if (cache[0].paddr == paddr) entry = &cache[0];
        if (cache[1].paddr == paddr) entry = &cache[1];

        if (!entry) {
        char* cmd = "./chroot-bin/find-nacl";
        char arg1[32];
        char arg2[32];
        int p = 0;
        int c, i;
        c = snprintf(arg1, sizeof(arg1), "%08lx", paddr & 0xffffffff);
        assert(c > 0);
        p = 0;
        for (i = 0; i < 8; i++) {
            c = snprintf(arg2+p, sizeof(arg2)-p, "%02x",
                         ((uint8_t*)&sig)[i]);
            assert(c > 0);
            p += c;
        }
        printf("cmd=%s %s %s\n", cmd, arg1, arg2);
        char buffer[256];
        char* args[] = {cmd, arg1, arg2, NULL};
        c = popen2(cmd, NULL, 0, buffer, 256, args);
        assert(c > 0);
        buffer[c] = 0;
        printf("buffer=%s\n", buffer);
        char* cut = strchr(buffer, ':');
        assert(cut);
        *cut = 0;
        int pid = atoi(buffer);
        char* file = cut+1;
        printf("pid=%d, file=%s\n", pid, file);

        entry = &cache[next_entry];
        entry->paddr = paddr;
        strncpy(entry->file, file, sizeof(entry->file));
        next_entry = (next_entry + 1) % 2;
        }

        int fdx = open(entry->file, O_RDWR);
        lseek(fdx, 0, SEEK_SET);
        write(fdx, img->data, size);
        close(fdx);
        printf("banzai!\n");
        /* Confirm write is done */
        write(fd, img->data, 8);
    } else {
        write(fd, img->data, size);
    }

    XDestroyImage(img);

    return 0;
}

int server_init() {
    struct sockaddr_in server_addr;
    int optval;

    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        syserror("Cannot create server socket.");
        exit(1);
    }

    /* SO_REUSEADDR to make sure the server can restart after a crash. */
    optval = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));

    /* Listen on loopback interface, port PORT. */
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    server_addr.sin_port = htons(PORT);

    if (bind(server_fd,
             (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        syserror("Cannot bind server socket.");
        exit(1);
    }

    if (listen(server_fd, 5) < 0) {
        syserror("Cannot listen on server socket.");
        exit(1);
    }

    printf("server_init\n");

    return 0;
}

int main(int argc, char** argv) {
    init_display();
    server_init();

    int newclient_fd;
    struct sockaddr_in client_addr;
    unsigned int client_addr_len = sizeof(client_addr);
    unsigned char buffer[BUFFERSIZE];

    while (1) {
        newclient_fd = accept(server_fd,
                          (struct sockaddr*)&client_addr, &client_addr_len);

        if (newclient_fd < 0) {
            syserror("Error accepting new connection.");
            return 1;
        }

        int flag = 1;
        setsockopt(newclient_fd,
                                 IPPROTO_TCP,
                                 TCP_NODELAY,
                                 (char*)&flag,
                                 sizeof(int));

        memset(cache, 0, sizeof(cache));

        int n;
        while ((n = read(newclient_fd, buffer, 8)) > 0) {
            //if (buffer[0] != 'S' && buffer[0] != 'M')
                printf("b %c:%02x%02x%02x%02x%02x%02x%02x\n",
                       buffer[0], buffer[1], buffer[2], buffer[3],
                       buffer[4], buffer[5], buffer[6], buffer[7]);
            switch (buffer[0]) {
            case 'S':
            {
                uint16_t width = *(uint16_t*)(buffer+1);
                uint16_t height = *(uint16_t*)(buffer+3);
                int shm = buffer[5];
                uint64_t paddr = 0;
                uint64_t sig = 0;
                if (shm) {
                    if (read(newclient_fd, &paddr, 8) != 8 ||
                        read(newclient_fd, &sig, 8) != 8) {
                        goto next_connection;
                    }
                    printf("P: %016lx\n", paddr);
                    printf("S: %016lx\n", sig);
                }
                write_image(newclient_fd, width, height, shm, paddr, sig);
            }
                break;
            case 'K':
            {
                KeySym ks = ((KeySym)buffer[2]) << 8 | buffer[3];
                KeyCode kc = XKeysymToKeycode(dpy, ks);
                printf("ks=%04x\n", (unsigned int)ks);
                printf("kc=%04x\n", kc);
                if (kc != 0) {
                    XTestFakeKeyEvent(dpy, kc, buffer[1], CurrentTime);
                } else {
                    fprintf(stderr, "Invalid keysym %04x.\n", (unsigned int)ks);
                }
            }
                break;
            case 'C':
            {
                int down = buffer[1];
                int button = buffer[2];
                XTestFakeButtonEvent(dpy, button, down, CurrentTime);
            }
                break;
            case 'M':
            {
                int x = ((KeySym)buffer[1]) << 8 | buffer[2];
                int y = ((KeySym)buffer[3]) << 8 | buffer[4];
                XTestFakeMotionEvent(dpy, 0, x, y, CurrentTime);
            }
                break;
            }
        }
    next_connection:
        close(newclient_fd);
    }

    return 0;
}
