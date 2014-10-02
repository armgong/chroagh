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
#include <X11/extensions/XTest.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <sys/shm.h>
#include <X11/extensions/XShm.h>
#include <X11/extensions/Xdamage.h>
#include <X11/extensions/Xfixes.h>

/* WebSocket constants */
#define VERSION "VF1"
#define PORT_BASE 30010

static Display *dpy;

int damageEvent;
int fixesEvent;

/* shm entry cache */
struct cache_entry {
    uint64_t paddr; /* Adress from PNaCl side */
    char file[256]; /* File to write to */
};

struct cache_entry cache[2];
int next_entry;

static int xerror_handler(Display *dpy, XErrorEvent *e) {
    return 0;
}

/* Register XDamage events for a given Window. */
static void register_damage(Display *dpy, Window win) {
    XWindowAttributes attrib;
    if (XGetWindowAttributes(dpy, win, &attrib)) {
        /* FIXME: Some sample code only cares if that's true, but this seems
         * to break i3... */
        if (!attrib.override_redirect) {
            XDamageCreate(dpy, win, XDamageReportRawRectangles);
        }
    }
}

static int init_display() {
    int event, error, major, minor;

    dpy = XOpenDisplay(NULL);

    if (!dpy) {
        error("Cannot open display.");
        return -1;
    }

    if (!XTestQueryExtension(dpy, &event, &error, &major, &minor)) {
        error("XTest not available!");
        return -1;
    }

    if (!XDamageQueryExtension(dpy, &damageEvent, &error)) {
        error("XDamage not available!");
        return -1;
    }

    if (!XFixesQueryExtension(dpy, &fixesEvent, &error)) {
        error("XFixes not available!");
        return -1;
    }

    Window root = DefaultRootWindow(dpy);
    Window rootp, parent;
    Window *children;
    unsigned int nchildren, i;

    /* Get notified when new windows are created. */
    XSelectInput(dpy, root, SubstructureNotifyMask);

    /* Register damage events for existing windows */
    XQueryTree(dpy, root, &rootp, &parent, &children, &nchildren);

    /* FIXME: We never reset the handler, is that a good thing? */
    XSetErrorHandler(xerror_handler);

    register_damage(dpy, root);
    for (i = 0; i < nchildren; i++) {
        register_damage(dpy, children[i]);
    }

    /* Register for cursor events */
    XFixesSelectCursorInput(dpy, root, XFixesDisplayCursorNotifyMask);

    return 0;
}

/* Change resolution using external handler.
 * Reply must be a resolution in "canonical" form: <w>x<h>[_<rate>] */
void change_resolution(const struct resolution* rin) {
    char* cmd = "findres";
    char arg1[32], arg2[32], buffer[256];
    int c;
    char* args[] = {cmd, arg1, arg2, NULL};
    char* endptr;

    c = snprintf(arg1, sizeof(arg1), "%d", rin->width);
    trueorabort(c > 0, "snprintf");
    c = snprintf(arg2, sizeof(arg2), "%d", rin->height);
    trueorabort(c > 0, "snprintf");
    log(2, "Running %s %s %s", cmd, arg1, arg2);
    c = popen2(cmd, args, NULL, 0, buffer, 256);
    trueorabort(c > 0, "popen2");

    buffer[c] = 0;
    log(2, "Result: %s", buffer);
    char* cut = strchr(buffer, '_');
    if (cut) *cut = 0;
    cut = strchr(buffer, 'x');
    trueorabort(cut, "Invalid answer: %s", buffer);
    *cut = 0;

    long nwidth = strtol(buffer, &endptr, 10);
    trueorabort(buffer != endptr && *endptr == '\0',
                    "Invalid width: '%s'", buffer);
    long nheight = strtol(cut+1, &endptr, 10);
    trueorabort(cut+1 != endptr && *endptr == '\0',
                    "Invalid height: '%s'", cut+1);
    log(1, "New resolution %ld x %ld", nwidth, nheight);

    char reply_raw[FRAMEMAXHEADERSIZE+sizeof(struct resolution)];
    struct resolution* r = (struct resolution*)(reply_raw+FRAMEMAXHEADERSIZE);
    r->type = 'R';
    r->width = nwidth;
    r->height = nheight;
    socket_client_write_frame(reply_raw, sizeof(*r), WS_OPCODE_BINARY, 1);
}

/* Find NaCl/Chromium shm memory using external handler.
 * Reply must be in the form PID:FD */
struct cache_entry* find_shm(uint64_t paddr, uint64_t sig) {
    struct cache_entry* entry = NULL;

    /* Find entry in cache */
    if (cache[0].paddr == paddr)
        entry = &cache[0];
    else if (cache[1].paddr == paddr)
        entry = &cache[1];

    if (!entry) {
        char* cmd = "find-nacl";
        char arg1[32], arg2[32], buffer[256];
        char* args[] = {cmd, arg1, arg2, NULL};
        int c, i, p = 0;
        char* endptr;

        c = snprintf(arg1, sizeof(arg1), "%08lx", (long)paddr & 0xffffffff);
        trueorabort(c > 0, "snprintf");
        p = 0;
        for (i = 0; i < 8; i++) {
            c = snprintf(arg2+p, sizeof(arg2)-p, "%02x",
                         ((uint8_t*)&sig)[i]);
            trueorabort(c > 0, "snprintf");
            p += c;
        }
        log(1, "Running %s %s %s", cmd, arg1, arg2);
        c = popen2(cmd, args, NULL, 0, buffer, 256);
        if (c <= 0) {
            error("Error running helper.");
            return NULL;
        }
        buffer[c] = 0;
        log(2, "Result: %s", buffer);

        char* cut = strchr(buffer, ':');
        if (!cut) {
            error("No : in helper reply: %s.", cut);
            return NULL;
        }
        *cut = 0;

        long pid = strtol(buffer, &endptr, 10);
        if(buffer == endptr || *endptr != '\0') {
            error("Invalid pid: %s", buffer);
            return NULL;
        }
        char* file = cut+1;
        log(1, "PID:%ld, FILE:%s", pid, file);

        entry = &cache[next_entry];
        entry->paddr = paddr;
        strncpy(entry->file, file, sizeof(entry->file));
        next_entry = (next_entry + 1) % 2;

        /* Open fd/mmap to file already. */
    }

    /* TODO: Check signature even if cache entry is valid */

    return entry;
}

XImage* img = NULL;
XShmSegmentInfo shminfo;

/* Write framebuffer image to websocket/shm */
int write_image(const struct screen* screen) {
    char reply_raw[FRAMEMAXHEADERSIZE+sizeof(struct screen_reply)];
    struct screen_reply* reply =
                           (struct screen_reply*)(reply_raw+FRAMEMAXHEADERSIZE);
    Window root = DefaultRootWindow(dpy);
    int refresh = 0;
    XEvent ev;

    /* Allocate XShmImage */
    if (!img || img->width != screen->width || img->height != screen->height) {
        if (img) {
            XDestroyImage(img);
            shmdt(shminfo.shmaddr);
            shmctl(shminfo.shmid, IPC_RMID, 0);
        }

        img = XShmCreateImage(dpy, DefaultVisual(dpy, 0), 24,
                              ZPixmap, NULL, &shminfo,
                              screen->width, screen->height);
        shminfo.shmid = shmget(IPC_PRIVATE, img->bytes_per_line * img->height, IPC_CREAT|0777);
        shminfo.shmaddr = img->data = (char*)shmat(shminfo.shmid, 0, 0);
        shminfo.readOnly = False;
        /* This may trigger the X protocol error we're ready to catch: */
        XShmAttach( dpy, &shminfo );
        /* Force refresh */
        refresh = 1000;
    }

    /* Register damage on new windows */
    while (XCheckTypedEvent(dpy, MapNotify, &ev)) {
        register_damage(dpy, ev.xcreatewindow.window);
        refresh++;
    }

    /* Check for damage */
    /* FIXME: Flush all events after the first one, as we don't care
     * about the rectangles... */
    while (XCheckTypedEvent(dpy, damageEvent+XDamageNotify, &ev)) {
        refresh++;
    }

    /* Check for cursor events */
    reply->cursor_updated = 0;
    while (XCheckTypedEvent(dpy, fixesEvent+XFixesCursorNotify, &ev)) {
        XFixesCursorNotifyEvent* curev = (XFixesCursorNotifyEvent*)&ev;
        char* name = XGetAtomName(dpy, curev->cursor_name);
        log(2, "cursor! %ld %s", curev->cursor_serial, name);
        XFree(name);
        reply->cursor_updated = 1;
        reply->cursor_serial = curev->cursor_serial;
    }

    /* No update? */
    if (!refresh) {
        reply->type = 'S';
        reply->shm = 0;
        reply->updated = 0;
        reply->width = screen->width;
        reply->height = screen->height;
        socket_client_write_frame(reply_raw, sizeof(*reply), WS_OPCODE_BINARY, 1);
        return 0;
    }

    /* Get new image from framebuffer */
    XShmGetImage(dpy, root, img, 0, 0, AllPlanes);

    int size = img->bytes_per_line * img->height;

    if (screen->shm) {
        struct cache_entry* entry = find_shm(screen->paddr, screen->sig);

        if (entry) {
            int fdx = open(entry->file, O_RDWR);
            lseek(fdx, 0, SEEK_SET);
            write(fdx, img->data, size);
            close(fdx);
        } else {
            error("Cannot find shm, maybe that's not a big deal...");
        }

        /* Confirm write is done */
        reply->type = 'S';
        reply->shm = 1;
        reply->updated = 1;
        reply->width = screen->width;
        reply->height = screen->height;
        socket_client_write_frame(reply_raw, sizeof(*reply), WS_OPCODE_BINARY, 1);
    } else {
        trueorabort(0, "Non-SHM path is currently broken!");
        /* Confirm write is done */
        reply->type = 'S';
        reply->shm = 0;
        reply->updated = 1;
        reply->width = screen->width;
        reply->height = screen->height;
        socket_client_write_frame(reply_raw, sizeof(*reply), WS_OPCODE_BINARY, 1);
        /* FIXME: This is broken with current API... */
        socket_client_write_frame(img->data, size, WS_OPCODE_BINARY, 1);
    }

    return 0;
}

/* Write cursor image to websocket */
int write_cursor() {
    XFixesCursorImage *img = XFixesGetCursorImage(dpy);
    int size = img->width*img->height;
    const int replylength = sizeof(struct cursor_reply)+sizeof(uint32_t)*size;
    char reply_raw[FRAMEMAXHEADERSIZE+replylength];
    struct cursor_reply* reply =
                           (struct cursor_reply*)(reply_raw+FRAMEMAXHEADERSIZE);
    int i = 0;

    reply->type = 'P';
    reply->width = img->width;
    reply->height = img->height;
    reply->xhot = img->xhot;
    reply->yhot = img->yhot;
    reply->cursor_serial = img->cursor_serial;
    /* This casts wasteful long* to uint32_t* */
    for (i = 0; i < size; i++)
        reply->pixels[i] = img->pixels[i];

#if 0
    /* Debug */
    int x, y;
    for (y = 0; y < img->height; y++) {
        for (x = 0; x < img->width; x++) {
            //printf("%08lx", img->pixels[y*img->width+x]);
            printf("%01x", (reply->pixels[y*img->width+x] >> 28) & 0xf);
        }
        printf("\n");
    }
#endif

    socket_client_write_frame(reply_raw, replylength, WS_OPCODE_BINARY, 1);
    XFree(img);

    return 0;
}

int check_size(int length, int target, char* error) {
    if (length != target) {
        error("Invalid %s packet (%d != %d)", error, length, target);
        socket_client_close(0);
        return 0;
    }
    return 1;
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
    socket_server_init(PORT_BASE); /* FIXME: Add display number to base */

    unsigned char buffer[BUFFERSIZE];
    int length;

    while (1) {
        struct key* k;
        struct mouseclick* mc;
        struct mousemove* mm;
        KeyCode kc;

        socket_server_accept(VERSION);
        memset(cache, 0, sizeof(cache));

        while (1) {
            length = socket_client_read_frame((char*)buffer, sizeof(buffer));
            if (length < 0) {
                socket_client_close(1);
                break;
            }

            if (length < 1) {
                error("Invalid packet from client (size <1).");
                socket_client_close(0);
                break;
            }

            switch (buffer[0]) {
            case 'S':
                if (!check_size(length, sizeof(struct screen), "screen"))
                    break;
                write_image((struct screen*)buffer);
                break;
            case 'P':
                if (!check_size(length, sizeof(struct cursor), "cursor"))
                    break;
                write_cursor();
                break;
            case 'R':
                if (!check_size(length, sizeof(struct resolution), "resolution"))
                    break;
                change_resolution((struct resolution*)buffer);
                break;
            case 'K':
                if (!check_size(length, sizeof(struct key), "key"))
                    break;
                k = (struct key*)buffer;
                kc = XKeysymToKeycode(dpy, k->keysym);
                log(2, "Key: ks=%04x kc=%04x\n", k->keysym, kc);
                if (kc != 0) {
                    XTestFakeKeyEvent(dpy, kc, k->down, CurrentTime);
                } else {
                    error("Invalid keysym %04x.", k->keysym);
                }
                break;
            case 'C':
                if (!check_size(length, sizeof(struct mouseclick), "mouseclick"))
                    break;
                mc = (struct mouseclick*)buffer;
                XTestFakeButtonEvent(dpy, mc->button, mc->down, CurrentTime);
                break;
            case 'M':
                if (!check_size(length, sizeof(struct mousemove), "mousemove"))
                    break;
                mm = (struct mousemove*)buffer;
                XTestFakeMotionEvent(dpy, 0, mm->x, mm->y, CurrentTime);
                break;
            default:
                error("Invalid packet from client (%d).", buffer[0]);
                socket_client_close(0);
            }
        }
        socket_client_close(0);
    }

    return 0;
}
