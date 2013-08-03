#!/bin/sh -e
# Copyright (c) 2013 The Chromium OS Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

# Grabs the release from the specified chroot ($1) and prints it out on stdout.
# Fails with an error code of 1 if the chroot does not belong to this distro.

if [ -z "$1" ]; then
    echo "Usage: ${0##*/} chroot" 1>&2
    exit 2
fi

os="${1%/}/etc/os-release"

if grep -q '^ID=arch$' "$os" 2>/dev/null; then
    echo "arch"
    exit 0
elif grep -q '^ID=archarm$' "$os" 2>/dev/null; then
    echo "alarm"
    exit 0
fi

exit 1
