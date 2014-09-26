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
#include <fcntl.h>
#include <sys/stat.h>
#include <netinet/tcp.h>
#include <assert.h>
#include <X11/extensions/XTest.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <sys/shm.h>
#include <X11/extensions/XShm.h>


/* WebSocket constants */
#define VERSION "VF1"
#define PORT_BASE 30010

static Display *dpy;

/* shm entry cache */
struct cache_entry {
    uint64_t paddr; /* Adress from PNaCl side */
    char file[256]; /* File to write to */
    off_t offset; /* offset to seek to */
};

struct cache_entry cache[2];
int next_entry;

int init_display() {
    dpy = XOpenDisplay(NULL);

    /* Fixme: check XTest extension available */

    return 0;
}

XImage* img = NULL;
XShmSegmentInfo shminfo;

int write_image(int width, int height,
                int shm, uint64_t paddr, uint64_t sig) {

    char reply[FRAMEMAXHEADERSIZE+8];

    /* TODO: Resize display as necessary */

    Window root = DefaultRootWindow(dpy);
    /* No Shm: */
    //XImage *img = XGetImage(dpy, root, 0, 0, width, height, AllPlanes, ZPixmap);
    //printf("size %d %d\n", img->bytes_per_line, img->height);

    if (!img || img->width != width || img->height != height) {
        if (img) {
            XDestroyImage( img );
            shmdt( shminfo.shmaddr );
            shmctl( shminfo.shmid, IPC_RMID, 0 );
        }

        img = XShmCreateImage( dpy, DefaultVisual(dpy, 0), 24,
                                   ZPixmap, NULL, &shminfo,
                                   width, height );
        shminfo.shmid = shmget( IPC_PRIVATE, img->bytes_per_line * img->height, IPC_CREAT|0777 );
        shminfo.shmaddr = img->data = (char*)shmat( shminfo.shmid, 0, 0 );
        shminfo.readOnly = False;
        /* This may trigger the X protocol error we're ready to catch: */
        XShmAttach( dpy, &shminfo );
    }

    XShmGetImage(dpy, root, img, 0, 0, AllPlanes);

    int size = img->bytes_per_line * img->height;

/*
    uint32_t* ptr = (uint32_t*)img->data;
    int i;
    for (i = 0; i < size/4; i++) {
        ptr[i] = (ptr[i] & 0x000000ff) << 16 |
                 (ptr[i] & 0x0000ff00) |
                 (ptr[i] & 0x00ff0000) >> 16 |
                 0xff000000;
    }
*/

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
            c = popen2(cmd, args, NULL, 0, buffer, 256);
            assert(c > 0);
            buffer[c] = 0;
            printf("buffer=%s\n", buffer);
            char* cut = strchr(buffer, ':');
            assert(cut);
            *cut = 0;
            int pid = atoi(buffer);
            char* file = cut+1;
            cut = strchr(file, ':');
            assert(cut);
            *cut = 0;
            off_t offset = strtol(cut+1, NULL, 10);
            printf("pid=%d, file=%s, offset=%lx\n", pid, file, offset);

            entry = &cache[next_entry];
            entry->paddr = paddr;
            strncpy(entry->file, file, sizeof(entry->file));
            entry->offset = offset;
            next_entry = (next_entry + 1) % 2;
        }

        int fdx = open(entry->file, O_RDWR);
        lseek(fdx, entry->offset, SEEK_SET);
        write(fdx, img->data, size);
        close(fdx);
        //printf("banzai!\n");
        /* Confirm write is done */
        reply[FRAMEMAXHEADERSIZE] = 'S';
        /* FIXME: Fill in info */
        socket_client_write_frame(reply, 8, WS_OPCODE_BINARY, 1);
    } else {
        reply[FRAMEMAXHEADERSIZE] = 'S';
        /* FIXME: Fill in info */
        socket_client_write_frame(reply, 8, WS_OPCODE_BINARY, 1);
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

            if (buffer[0] != 'S' && buffer[0] != 'M' && verbose)
                printf("b %d %c:%02x%02x%02x%02x%02x%02x%02x\n",
                   length,
                   buffer[0], buffer[1], buffer[2], buffer[3],
                   buffer[4], buffer[5], buffer[6], buffer[7]);

            switch (buffer[0]) {
            case 'S':
            {
                int shm = buffer[1];
                uint16_t width = *(uint16_t*)(buffer+2);
                uint16_t height = *(uint16_t*)(buffer+4);
                uint64_t paddr = 0;
                uint64_t sig = 0;
                if (shm && length == 24) {
                    paddr = *(uint64_t*)(buffer+8);
                    sig = *(uint64_t*)(buffer+16);
                    //printf("P: %016lx\n", paddr);
                    //printf("S: %016lx\n", sig);
                }
                write_image(width, height, shm, paddr, sig);
            }
                break;
            case 'K':
            {
                KeySym ks = *(uint16_t*)(buffer+2);
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
                int x = *(uint16_t*)(buffer+1);
                int y = *(uint16_t*)(buffer+3);
                XTestFakeMotionEvent(dpy, 0, x, y, CurrentTime);
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
