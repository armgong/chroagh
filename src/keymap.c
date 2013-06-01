/* Copyright (c) 2013 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 *
 * Monitors the X11 server for keboard layout change events, and copies the
 * layout over to the X11 server specified as parameter.
 *
 * Compile with: gcc keymap.c -lX11 -lxkbfile -o keymap
 */

#include <stdio.h>
#include <unistd.h>
#include <X11/Xlib.h>
#include <X11/XKBlib.h>
#include <X11/extensions/XKBrules.h>

static int verbose = 0;
static int error = 0;
static Display *cros_d;
static Display *chroot_d;

static int error_handler(Display *d, XErrorEvent *e) {
    fprintf(stderr, "X11 error: %d, %d, %d\n",
            e->error_code, e->request_code, e->minor_code);
    error = 1;
    return 0;
}

static int forward_layout() {
    XkbRF_VarDefsRec cros_vdr, chroot_vdr;
    char* tmp;

    if (!XkbRF_GetNamesProp(cros_d, &tmp, &cros_vdr)) {
        fprintf(stderr, "Failed to obtain Xkb names from Chromium OS.");
        return 0;
    }

    if (verbose) {
        printf("Chromium OS: model: %s; layout: %s; variant: %s; options: %s; tmp: %s\n",
            cros_vdr.model, cros_vdr.layout, cros_vdr.variant, cros_vdr.options, tmp);
    }

    if (!XkbRF_GetNamesProp(chroot_d, &tmp, &chroot_vdr)) {
        fprintf(stderr, "Failed to obtain Xkb names from chroot display.");
        return 0;
    }

    chroot_vdr.layout = cros_vdr.layout;
    chroot_vdr.variant = cros_vdr.variant;
    chroot_vdr.options = cros_vdr.options;

    if (!XkbRF_SetNamesProp(chroot_d, tmp, &chroot_vdr)) {
        fprintf(stderr, "Failed to set Xkb names to chroot display.");
        return 0;
    }

    return 1;
}

int main(int argc, char** argv) {
    if (argc != 2 || !argv[1][0] || !argv[1][1]) {
        fprintf(stderr, "Usage: %s chrootdisplay\n", argv[0]);
        return 2;
    }
    /* Make sure the displays aren't equal */
    char *cros_n = XDisplayName(NULL);
    if (cros_n[1] == argv[1][1]) {
        fprintf(stderr, "You must specify a different display.\n");
        return 2;
    }
    /* Open the displays */
    int cros_xkbEventCode = 0;
    if (!(cros_d = XkbOpenDisplay(NULL, &cros_xkbEventCode, NULL, NULL, NULL, NULL))) {
        fprintf(stderr, "Failed to open Chromium OS display\n");
        return 1;
    }
    if (!(chroot_d = XOpenDisplay(argv[1]))) {
        fprintf(stderr, "Failed to open chroot display %s\n", argv[1]);
        return 1;
    }

    XSetErrorHandler(error_handler);

    /* Synchronize at startup */
    forward_layout(cros_d, chroot_d);

    /* Listen for events */
    if (!XkbSelectEvents(cros_d, XkbUseCoreKbd, XkbNewKeyboardNotifyMask, XkbNewKeyboardNotifyMask)) {
        fprintf(stderr, "Failed to select XKB events.\n");
        return 1;
    }

    XkbEvent event;
    while (!error) {
        XNextEvent(cros_d, &event.core);
        if (error) break;
        if (event.type != cros_xkbEventCode) continue;

        if (event.any.xkb_type == XkbNewKeyboardNotify) {
            /* FIXME: We receive 4 events per layout switch (one per input device?), can we filter that out? */
            if (verbose) printf("XkbNewKeyboardNotify (dev=%x time=%ld)!\n", event.any.device, event.any.time);
            /* FIXME: We receice the event before the layout is actually modified... Is there a better way than sleeping? */
            usleep(100*1000);
            forward_layout(cros_d, chroot_d);
        } else {
            if (verbose) printf("Event (%08x)!\n", event.any.xkb_type);
        }
    }

    XCloseDisplay(cros_d);
    XCloseDisplay(chroot_d);
    return 0;
}
