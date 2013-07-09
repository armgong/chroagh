/* Copyright (c) 2013 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 *
 * Adapted from cursor.c, itself partly adapted from the XcursorImageLoadCursor
 * implementation in libXcursor, copyright 2002 Keith Packard.
 *
 * Monitors the X11 server for keboard layout change events, and copies the
 * layout over to the X11 server specified as parameter.
 *
 * We only support "standard" keymaps, and not languages requiring special
 * handling by ibus (e.g. Vietnamese, Chinese...).
 *
 *
 * Compile with: gcc keymap.c -lX11 -lxkbfile -o keymap
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
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

/* Unbelievably, XkbRF_GetNamesProp lacks a proper way to free its returned
 * structures... */
static void FreeVarDefsRec(XkbRF_VarDefsRec *vdp) {
    free(vdp->model);
    free(vdp->layout);
    free(vdp->variant);
    free(vdp->options);
}

/* Duplicated libxkbfile routine in libxbkfile/src/XKBfileInt.h */
static inline
char *_XkbDupString(const char *s)
{
    return s ? strdup(s) : NULL;
}

static int forward_layout() {
    XkbRF_VarDefsRec cros_vdr, newchroot_vdr, chroot_vdr;
    char* cros_rf;
    char *chroot_rf;

    if (!XkbRF_GetNamesProp(cros_d, &cros_rf, &cros_vdr)) {
        fprintf(stderr, "Failed to obtain Xkb names from Chromium OS.");
        return 0;
    }

    if (verbose) {
        printf("Chromium OS: model: %s; layout: %s; variant: %s; "
               "options: %s; rulefile: %s\n",
            cros_vdr.model, cros_vdr.layout, cros_vdr.variant,
            cros_vdr.options, cros_rf);
    }

    if (!XkbRF_GetNamesProp(chroot_d, &chroot_rf, &chroot_vdr)) {
        fprintf(stderr, "Failed to obtain Xkb names from chroot display.");
        return 0;
    }

    /* New keyboard description: Keep model, update the rest. */
    newchroot_vdr.model = _XkbDupString(chroot_vdr.model);
    newchroot_vdr.layout = _XkbDupString(cros_vdr.layout);
    newchroot_vdr.variant = _XkbDupString(cros_vdr.variant);
    newchroot_vdr.options = _XkbDupString(cros_vdr.options);

    if (!XkbRF_SetNamesProp(chroot_d, chroot_rf, &chroot_vdr)) {
        fprintf(stderr, "Failed to set Xkb names to chroot display.");
        return 0;
    }

    FreeVarDefsRec(&cros_vdr);
    FreeVarDefsRec(&chroot_vdr);
    FreeVarDefsRec(&newchroot_vdr);
    free(cros_rf);
    free(chroot_rf);

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
    if (!(cros_d = XOpenDisplay(NULL))) {
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

    Window cros_w = DefaultRootWindow(cros_d);

    /* An (more proper) alternative is to listen for XkbNewKeyboardNotify
     * events. However, these events are sent before the keymap (or at least
     * the property) is actually changed, and is called once per input device.
     * Listening for PropertyChange in _XKB_RF_NAMES_PROP_ATOM does not
     * cause these problems, but may be more likely to break in case of
     * internal Xorg changes. */
    XSelectInput(cros_d, cros_w, PropertyChangeMask);

    XEvent event;
    while (!error) {
        XNextEvent(cros_d, &event);
        if (error) break;
        if (event.type != PropertyNotify) continue;

        char* name = XGetAtomName(cros_d, event.xproperty.atom);

        if (verbose) printf("PropertyNotify (%s)!\n", name);

        if (name && strcmp(name, _XKB_RF_NAMES_PROP_ATOM) == 0) {
            forward_layout(cros_d, chroot_d);
        }

        XFree(name);
    }

    XCloseDisplay(cros_d);
    XCloseDisplay(chroot_d);
    return 0;
}
