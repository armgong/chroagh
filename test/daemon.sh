#!/bin/sh -e
# Copyright (c) 2014 The crouton Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

# Monitors a web-based CSV queue for autotest requests, runs the test, and
# uploads the status and results.

# Example CSV contents (must be fully quoted):
#   "Timestamp","Repository","Branch","Additional parameters"
#   "2013/10/16 8:24:52 PM GMT","dnschneid/crouton","master",""

set -e

APPLICATION="${0##*/}"
SCRIPTDIR="`readlink -f "\`dirname "$0"\`/.."`"
LOGUPLOADINTERVAL=60
POLLINTERVAL=10
QUEUEURL=''
RSYNCBASEOPTIONS='-aP'
RSYNCOPTIONS='--delete'
UPLOADROOT="$HOME"
AUTOTESTGIT="https://chromium.googlesource.com/chromiumos/third_party/autotest"
TESTINGSSHKEYURL="https://chromium.googlesource.com/chromiumos/chromite/+/master/ssh_keys/testing_rsa"
USER="drinkcat"
GSCROUTONTEST="gs://drinkcat-crouton/crouton-test/packages"
MIRRORENV=""

USAGE="$APPLICATION [options] -q QUEUEURL

Runs a daemon that polls a CSV on the internet for tests to run, and uploads
status and results to some destination via scp.

Options:
    -e MIRRORENV   key=value pair to pass to tests to setup default mirrors.
                   Can be specified multiple times.
    -q QUEUEURL    Queue URL to poll for new test requests. Must be specified.
    -r RSYNCOPT    Special options to pass to rsync in addition to $RSYNCBASEOPTIONS
                   Default: ${RSYNCOPTIONS:-"(nothing)"}
    -u UPLOADROOT  Base rsync-compatible URL directory to upload to. Must exist.
                   Default: $UPLOADROOT"

# Common functions
. "$SCRIPTDIR/installer/functions"

# Process arguments
while getopts 'e:q:s:u:' f; do
    case "$f" in
    e) MIRRORENV="$OPTARG;$MIRRORENV";;
    q) QUEUEURL="$OPTARG";;
    r) RSYNCOPTIONS="$OPTARG";;
    u) UPLOADROOT="$OPTARG";;
    \?) error 2 "$USAGE";;
    esac
done
shift "$((OPTIND-1))"

if [ -z "$QUEUEURL" -o "$#" != 0 ]; then
    error 2 "$USAGE"
fi

# Find a board name from a given host name
# Also creates a host info file, that is used by findrelease
findboard() {
    local host="$1"

    local hostinfo="$HOSTINFO/$host"

    if [ ! -s "$hostinfo" ]; then
        echo '
echo
echo "HWID=`crossystem hwid`"
cat /etc/lsb-release
' | ssh "root@${host}.cros" -o IdentityFile="$SSHKEY" \
        -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
        -o ConnectTimeout=30 > "$hostinfo"
    fi

    sed -n 's/^CHROMEOS_RELEASE_BOARD=//p' "$hostinfo"
}

# Find a release/build name from host, board and channel
findrelease() {
    local host="$1"
    local channel="$2"
    local board="`findboard "$host"`"
    local hostinfo="$HOSTINFO/$host"

    local appidtype="RELEASE"
    if [ "$channel" = "canary" ]; then
        appidtype="CANARY"
    fi
    local appid="`sed -n "s/^CHROMEOS_${appidtype}_APPID=//p" "$hostinfo"`"
    local hwid="`sed -n 's/^HWID=//p' "$hostinfo"`"

    tee /dev/stderr <<EOF | curl -d @- https://tools.google.com/service/update2 | tee /dev/stderr | tr '<' '\n' | sed -n 's/^action.*ChromeOSVersion="\([^"]*\)".*ChromeVersion="\([0-9]*\)\..*/R\2-\1/p'
<?xml version="1.0" encoding="UTF-8"?>
<request protocol="3.0" version="ChromeOSUpdateEngine-0.1.0.0"
                 updaterversion="ChromeOSUpdateEngine-0.1.0.0">
    <os version="Indy" platform="Chrome OS"></os>
    <app appid="${appid}" version="0.0.0" track="${channel}-channel"
         lang="en-US" board="${board}" hardware_class="${hwid}"
         delta_okay="false" fw_version="" ec_version="" installdate="2800" >
        <updatecheck targetversionprefix=""></updatecheck>
        <event eventtype="3" eventresult="2" previousversion=""></event>
    </app>
</request>
EOF
}

lastfullsync=0
forceupdate=
# Sync status directory
# Setting a parameter will sync that specific file only
syncstatus() {
    file="$1"
    if [ -z "$file" ]; then
        if [ -z "$forceupdate" ]; then
            time="`date '+%s'`"
            if [ "$((lastfullsync+LOGUPLOADINTERVAL))" -gt "$time" ]; then
                echo "Skipping sync (throttling)..."
                return
            fi
            lastfullsync="$time"
        else
            forceupdate=
        fi
    fi
    echo "Syncing $file"
    rsync $RSYNCBASEOPTIONS $RSYNCOPTIONS "$STATUSROOT/$file" \
          "$UPLOADROOT/crouton-autotest/$file" >/dev/null 2>&1
}

log() {
    timestamp="`TZ= date +"%Y-%m-%d %H:%M:%S.%N"`"
    echo "$timestamp:$@" | tee -a "$STATUSROOT/status"
    syncstatus status
}

TMPROOT="`mktemp -d --tmpdir='/tmp' 'crouton-autotest.XXX'`"
addtrap "rm -rf --one-file-system '$TMPROOT'"
LASTFILE="$TMPROOT/last"
echo "2 `date '+%s'`" > "$LASTFILE"
HOSTINFO="$TMPROOT/hostinfo"
mkdir -p "$HOSTINFO"

# Persistent storage directory
LOCALROOT="$SCRIPTDIR/test/daemon"

# status directory: synced via rsync
STATUSROOT="$LOCALROOT/status"
mkdir -p "$STATUSROOT"

log "crouton autotest daemon starting..."

echo "Fetching latest autotest..."
AUTOTESTROOT="$LOCALROOT/autotest.git"
if [ -d "$AUTOTESTROOT/.git" ]; then
    (
        cd "$AUTOTESTROOT"
        git fetch
        git reset --hard origin/master >/dev/null
    )
else
    git clone "$AUTOTESTGIT" "$AUTOTESTROOT"
fi
PATH="$AUTOTESTROOT/cli:$PATH"

echo "Checking if gsutil is installed..."
gsutil version

echo "Fetching testing ssh keys..."
SSHKEY="$LOCALROOT/testing_rsa"
wget "$TESTINGSSHKEYURL?format=TEXT" -O- | base64 -d > "$SSHKEY"
chmod 0600 "$SSHKEY"

echo "Building latest test-plaform_crouton tarball..."
tar cvfj "$LOCALROOT/test-platform_crouton.tar.bz2" \
    -C "$SCRIPTDIR/test/autotest/platform_crouton" . --exclude='*~'
( cd "$LOCALROOT"; md5sum test-platform_crouton.tar.bz2 > packages.checksum )
TESTCSUM="`cat "$LOCALROOT/packages.checksum" | cut -d' ' -f 1`"
# TODO: md5sum adds an extra space, is that harmful?
gsutil cp "$LOCALROOT"/test-platform_crouton.tar.bz2 \
          "$LOCALROOT"/packages.checksum "$GSCROUTONTEST-$TESTCSUM/"

syncstatus

while sleep "$POLLINTERVAL"; do
    read lastline last < "$LASTFILE"
    # Grab the queue, skip to the next interesting line, convert field
    # boundaries into pipe characters, and then parse the result.
#    (cat queue && echo) \
    (wget -qO- "$QUEUEURL" && echo) \
            | tail -n"+$lastline" | sed 's/^"//; s/","/|/g; s/"$//' | {
        while IFS='|' read date repo branch params runtype _; do
            if [ -z "$date" ]; then
                continue
            fi
            lastline="$(($lastline+1))"
            # Convert to UNIX time and skip if it's an old request
            date="`date '+%s' --date="$date"`"
            if [ "$date" -le "$last" ]; then
                continue
            fi
            last="$date"

            # Validate the other fields
            branch="${branch%%/*}"
            gituser="${repo%%/*}"
            repo="${repo##*/}"

            date="`date -u '+%Y-%m-%d_%H-%M-%S' --date="@$date"`"
            paramsstr="${params:+"_"}`echo "$params" | tr ' [:punct:]' '_-'`"
            tname="${date}_${gituser}_${repo}_$branch$paramsstr"
            curtestroot="$STATUSROOT/$tname"

            channels=""
            if [ "${runtype#full}" != "$runtype" ]; then
                channels="stable beta dev"
                if [ "${runtype#full+canary}" != "$runtype" ]; then
                    channels="$channels canary"
                fi
            fi

            mkdir -p "$curtestroot"

            log "$tname *: Dispatching"

            hostlist="`atest host list -w cautotest \
                                       -N -b pool:crouton --unlocked || true`"

            if [ -z "$hostlist" ]; then
                log "$tname *: Failed to retrieve host list"
                continue
            fi

            for host in $hostlist; do
                if [ -z "$channels" ]; then
                    # Use atest labels to select which channels to run tests on
                    channels="`atest host list --parse "$host" | \
                               sed -n 's/.*|Labels=\([^|]*\).*/\1/p' | \
                               tr ',' '\n' | sed -n 's/ *crouton://p'`"
                    if [ -z "$channels" ]; then
                        log "ERROR: No default channel configured for $host."
                        continue
                    fi
                fi
                for channel in $channels; do
                    # Abbreviation of host name
                    hostshort="`echo "$host" | sed -e 's/-*\([a-z]\)[a-z]*/\1/g'`"

                    board="`findboard "$host" || true`"
                    hostfull="$hostshort-$board-$channel"
                    if [ -z "$board" ]; then
                        log "$tname $hostfull: ERROR cannot find board name!"
                        continue
                    fi

                    release="`findrelease "$host" "$channel" || true`"
                    if [ -z "$release" ]; then
                        log "$tname $hostfull: ERROR cannot find release name!"
                        continue
                    fi

                    curtesthostroot="$curtestroot/$hostfull"

                    if [ -d "$curtesthostroot" ]; then
                        log "$tname $hostfull: Already started"
                        continue
                    fi

                    mkdir -p "$curtesthostroot"

                    # Generate control file
                    sed -e "s ###REPO### $gituser/$repo " \
                        -e "s ###BRANCH### $branch " \
                        -e "s;###RUNARGS###;$params;" \
                        -e "s|###ENV###|$MIRRORENV|" \
                         -e "s|###TESTCSUM###|$TESTCSUM|" \
                        $SCRIPTDIR/test/autotest/control.template \
                        > "$curtesthostroot/control"

                    ret=
                    (
                        set -x
                        atest job create -m "$host" -w cautotest \
                            -f "$curtesthostroot/control" \
                            -d "cros-version:${board}-release/$release" \
                            "$tname-$hostfull"
                    ) > "$curtesthostroot/atest" 2>&1 || ret=$?

                    if [ -z "$ret" ]; then
                        cat "$curtesthostroot/atest" | tr '\n' ' ' | \
                        sed -e 's/^.*(id[^0-9]*\([0-9]*\)).*$/\1/' \
                        > "$curtesthostroot/jobid"
                    else
                        log "$tname $hostfull: Create job failed"
                    fi
                    forceupdate=y
                done # channel
            done # host
            syncstatus
        done
        echo "$lastline $last" > "$LASTFILE"
    }

    # Check status of running tests
    for curtestroot in "$STATUSROOT"/*; do
        testupdated=
        curtest="${curtestroot#$STATUSROOT/}"
        for curtesthostroot in "$curtestroot"/*; do
            curtesthost="${curtesthostroot#$curtestroot/}"
            if [ -f "$curtesthostroot/jobid" ]; then
                jobid="`cat "$curtesthostroot/jobid"`"
                newstatusfile="$curtesthostroot/newstatus"
                statusfile="$curtesthostroot/status"
                if ! atest job stat "$jobid" > "$newstatusfile"; then
                    log "$curtest $curtesthost: Cannot get status."
                    continue
                fi
                status="`awk 'NR==2{sub(/=.*$/,"",$4);print $4;exit};0' \
                             "$newstatusfile"`"
                if ! diff -q "$newstatusfile" "$statusfile" >/dev/null 2>&1; then
                    log "$curtest $curtesthost: $status"
                    testupdated=y
                    mv "$newstatusfile" "$statusfile"
                else
                    rm -f "$newstatusfile"
                fi

                # TODO: If status is Running, we can rsync from the host directly

                # Any more final statuses?
                if [ "$status" = "Aborted" -o "$status" = "Failed" \
                                           -o "$status" = "Completed" ]; then
                    # It may take a while for the files to be transfered, retry.
                    if ! root="`gsutil ls "gs://chromeos-autotest-results/$jobid-$USER"`"; then
                        # Cannot fetch for now, try again later...
                        continue
                    fi
                    for path in "debug/*" "platform_crouton/debug/*" \
                                "platform_crouton/results/*"; do
                        # FIXME: Can we prevent partial fetches???
                        gsutil cp "${root}${path}" "$curtesthostroot" \
                            > /dev/null 2>&1 || true
                    done
                    log "$curtest $curtesthost: $status (fetched)"
                    echo "daemon: Fetched results." >> "$statusfile"
                    testupdated=y
                    rm $curtesthostroot/jobid
                fi
            fi
        done
        if [ -n "$testupdated" ]; then
            ( cd "$curtestroot"; tail -n +1 */status > status )
            forceupdate=y
        fi
    done

    hostlist="`atest host list -w cautotest -N -b pool:crouton`"
    for host in $hostlist; do
        atest host list -w cautotest "$host" || true
        # FIXME: This command is really slow, why?
        #atest host jobs -w cautotest "$host"
        echo "======="
    done > "$STATUSROOT/newhoststatus"
    if ! diff -q "$STATUSROOT/newhoststatus" \
                 "$STATUSROOT/hoststatus" >/dev/null 2>&1; then
        mv "$STATUSROOT/newhoststatus" "$STATUSROOT/hoststatus"
    else
        rm -f "$STATUSROOT/newhoststatus"
    fi

    syncstatus
done
