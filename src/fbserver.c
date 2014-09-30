/* Copyright (c) 2014 The crouton Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 *
 * WebSocket server that provides an interface to an extension running in
 * Chromium OS, used for clipboard synchronization and URL handling.
 *
 */

#define _GNU_SOURCE /* for ppoll */
#include "websocket.h"
#include "fbserver-proto.h"
#include <fcntl.h>
#include <sys/stat.h>
#include <netinet/tcp.h>
#include <assert.h>
#include <X11/extensions/XTest.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <sys/shm.h>
#include <X11/extensions/XShm.h>
#include <X11/extensions/Xdamage.h>

/* WebSocket constants */
#define VERSION "VF1"
#define PORT_BASE 30010

static Display *dpy;

int damageEvent;

/* shm entry cache */
struct cache_entry {
    uint64_t paddr; /* Adress from PNaCl side */
    char file[256]; /* File to write to */
};

struct cache_entry cache[2];
int next_entry;

static int xerrorHandler(Display *dpy, XErrorEvent *e) {
    return 0;
}

static void registerdamage(Display *dpy, Window win) {
    XWindowAttributes attrib;
    if (XGetWindowAttributes(dpy, win, &attrib)) {
        /* FIXME: Some sample code only cares if that's true, but this seems
         * to break i3... */
        if (!attrib.override_redirect) {
            Damage xdamage = XDamageCreate(dpy, win,
                                           XDamageReportRawRectangles);
            /* FIXME: Necessary? */
            XDamageSubtract(dpy, xdamage, None, None);
        }
    }
}

static int init_display() {
    dpy = XOpenDisplay(NULL);

    /* Fixme: check XTest extension available */

    int damageError;

    if (!XDamageQueryExtension (dpy, &damageEvent, &damageError)) {
        fprintf(stderr, "XDamage not available!\n");
        return -1;
    }

    Window root, parent;
    Window *children;
    unsigned int nchildren, i;

    XSelectInput(dpy, XRootWindow(dpy, 0), SubstructureNotifyMask);

    XQueryTree(dpy, XRootWindow(dpy, 0), &root, &parent,
               &children, &nchildren);

    XSetErrorHandler(xerrorHandler);

    registerdamage(dpy, XRootWindow(dpy, 0));
    for (i = 0; i < nchildren; i++) {
        registerdamage(dpy, children[i]);
    }

    return 0;
}

void change_resolution(int width, int height) {
    char* cmd = "./chroot-bin/findres";
    char arg1[32];
    char arg2[32];
    int c;
    c = snprintf(arg1, sizeof(arg1), "%d", width);
    assert(c > 0);
    c = snprintf(arg2, sizeof(arg2), "%d", height);
    assert(c > 0);
    printf("cmd=%s %s %s\n", cmd, arg1, arg2);
    char buffer[256];
    char* args[] = {cmd, arg1, arg2, NULL};
    c = popen2(cmd, args, NULL, 0, buffer, 256);
    assert(c > 0);
    buffer[c] = 0;
    printf("buffer=%s\n", buffer);
    char* cut = strchr(buffer, '_');
    if (cut) *cut = 0;
    cut = strchr(buffer, 'x');
    assert(cut);
    *cut = 0;
    int nwidth = atoi(buffer);
    int nheight = atoi(cut+1);
    printf("Resolution %d x %d\n", nwidth, nheight);

    char reply_raw[FRAMEMAXHEADERSIZE+sizeof(struct resolution)];
    struct resolution* reply = (struct resolution*)(reply_raw+FRAMEMAXHEADERSIZE);
    reply->type = 'R';
    reply->width = nwidth;
    reply->height = nheight;
    socket_client_write_frame(reply_raw, sizeof(*reply), WS_OPCODE_BINARY, 1);
}

struct cache_entry* find_shm(uint64_t paddr, uint64_t sig) {
    struct cache_entry* entry = NULL;

    if (cache[0].paddr == paddr) entry = &cache[0];
    if (cache[1].paddr == paddr) entry = &cache[1];

    /* TODO: Check sig */

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
        c = popen2(cmd, args, NULL, 0, buffer, 256);
        if (c <= 0) {
            return NULL;
        }
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

    return entry;
}

XImage* img = NULL;
XShmSegmentInfo shminfo;

int write_image(int width, int height,
                int shm, uint64_t paddr, uint64_t sig) {

    char reply_raw[FRAMEMAXHEADERSIZE+sizeof(struct screen_reply)];
    struct screen_reply* reply = (struct screen_reply*)(reply_raw+FRAMEMAXHEADERSIZE);

    /* TODO: Resize display as necessary */

    Window root = DefaultRootWindow(dpy);
    /* No Shm: */
    //XImage *img = XGetImage(dpy, root, 0, 0, width, height, AllPlanes, ZPixmap);
    //printf("size %d %d\n", img->bytes_per_line, img->height);

    if (!img || img->width != width || img->height != height) {
        if (img) {
            XDestroyImage(img);
            shmdt(shminfo.shmaddr);
            shmctl(shminfo.shmid, IPC_RMID, 0);
        }

        img = XShmCreateImage(dpy, DefaultVisual(dpy, 0), 24,
                              ZPixmap, NULL, &shminfo,
                              width, height);
        shminfo.shmid = shmget(IPC_PRIVATE, img->bytes_per_line * img->height, IPC_CREAT|0777);
        shminfo.shmaddr = img->data = (char*)shmat(shminfo.shmid, 0, 0);
        shminfo.readOnly = False;
        /* This may trigger the X protocol error we're ready to catch: */
        XShmAttach( dpy, &shminfo );
    }

    /* Check for damage */
    int refresh = 0;
    XEvent ev;

    while (XCheckTypedEvent(dpy, MapNotify, &ev)) {
        registerdamage(dpy, ev.xcreatewindow.window);
        refresh++;
    }

    /* Flush all events after the first one, if we don't care
     * about the rectangles... */
    while (XCheckTypedEvent(dpy, damageEvent+XDamageNotify, &ev)) {
        refresh++;
    }

    if (!refresh) {
        reply->type = 'S';
        reply->shm = 0;
        reply->updated = 0;
        reply->width = width;
        reply->height = height;
        socket_client_write_frame(reply_raw, sizeof(*reply), WS_OPCODE_BINARY, 1);
        return 0;
    }

/*    if (refresh)
      printf("refresh=%d\n", refresh);*/

    XShmGetImage(dpy, root, img, 0, 0, AllPlanes);

    int size = img->bytes_per_line * img->height;

    if (shm) {
        struct cache_entry* entry = find_shm(paddr, sig);

        if (entry) {
            int fdx = open(entry->file, O_RDWR);
            lseek(fdx, 0, SEEK_SET);
            write(fdx, img->data, size);
            close(fdx);
        } else {
            printf("Cannot find shm, maybe that's not a big deal...\n");
        }
        //printf("banzai!\n");
        /* Confirm write is done */
        reply->type = 'S';
        reply->shm = 1;
        reply->updated = 1;
        reply->width = width;
        reply->height = height;
        socket_client_write_frame(reply_raw, sizeof(*reply), WS_OPCODE_BINARY, 1);
    } else {
        reply->type = 'S';
        reply->shm = 0;
        reply->updated = 1;
        reply->width = width;
        reply->height = height;
        socket_client_write_frame(reply_raw, sizeof(*reply), WS_OPCODE_BINARY, 1);
        /* FIXME: This is broken with current API... */
        socket_client_write_frame(img->data, size, WS_OPCODE_BINARY, 1);
    }

    //XDestroyImage(img);

    return 0;
}

int main(int argc, char** argv) {
    int c;

    while ((c = getopt(argc, argv, "v:")) != -1) {
        switch (c) {
        case 'v':
            verbose = atoi(optarg);
            break;
        default:
            fprintf(stderr, "%s [-v 0-3]\n", argv[0]);
            return 1;
        }
    }

    init_display();
    socket_server_init(PORT_BASE); /* Add display number */

    unsigned char buffer[BUFFERSIZE];
    int length;

    while (1) {
        socket_server_accept(VERSION);
        memset(cache, 0, sizeof(cache));

        while (1) {
            length = socket_client_read_frame((char*)buffer, sizeof(buffer));
            if (length < 0) {
                socket_client_close(1);
                break;
            }

            /* FIXME: Check buffer length */
/*
            if (buffer[0] != 'S' && buffer[0] != 'M' && verbose)
                printf("b %d %c:%02x%02x%02x%02x%02x%02x%02x\n",
                   length,
                   buffer[0], buffer[1], buffer[2], buffer[3],
                   buffer[4], buffer[5], buffer[6], buffer[7]);
*/

            switch (buffer[0]) {
            case 'S':
            {
                const struct screen* s = (struct screen*)buffer;
                write_image(s->width, s->height, s->shm, s->paddr, s->sig);
            }
                break;
            case 'R':
            {
                const struct resolution* r = (struct resolution*)buffer;
                change_resolution(r->width, r->height);
            }
                break;
            case 'K':
            {
                const struct key* k = (struct key*)buffer;
                KeyCode kc = XKeysymToKeycode(dpy, k->keysym);
                printf("ks=%04x\n", k->keysym);
                printf("kc=%04x\n", kc);
                if (kc != 0) {
                    XTestFakeKeyEvent(dpy, kc, k->down, CurrentTime);
                } else {
                    fprintf(stderr, "Invalid keysym %04x.\n", k->keysym);
                }
            }
                break;
            case 'C':
            {
                const struct mouseclick* mc = (struct mouseclick*)buffer;
                XTestFakeButtonEvent(dpy, mc->button, mc->down, CurrentTime);
            }
                break;
            case 'M':
            {
                const struct mousemove* mm = (struct mousemove*)buffer;
                XTestFakeMotionEvent(dpy, 0, mm->x, mm->y, CurrentTime);
            }
                break;
            default:
                error("Invalid packet from client.");
                socket_client_close(0);
            }
        }
        socket_client_close(0);
    }

    return 0;
}
