// Copyright (c) 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

CriatModule = null;  // Global application object.
statusText = 'NO-STATUS';
listener_ = null;

// Indicate load success.
function moduleDidLoad() {
    CriatModule = document.getElementById('criat');
    updateStatus('SUCCESS');
    // Send a message to the Native Client module
    handleResize();
}

function pageDidLoad() {
    if (CriatModule == null) {
        updateStatus('LOADING...');
    } else {
        updateStatus();
    }
}

function updateStatus(opt_message) {
    if (opt_message)
        statusText = opt_message;
    var statusField = document.getElementById('statusField');
    if (statusField) {
        statusField.innerHTML = statusText;
    }
}

// This function is called by common.js when a message is received from the
// NaCl module.
function handleMessage(message) {
    var str = message.data;
    var type, payload, i;
    if ((i = str.indexOf(":")) > 0) {
        type = str.substr(0, i);
        payload = str.substr(i+1);
    } else {
        type = "log";
        payload = str;
    }

    if (type == "log") {
        var logEl = document.getElementById('log');
        logEl.textContent = message.data;
        console.log(message.data);
    } else if (type == "resize") {
        i = payload.indexOf("/");
        if (i < 0) return;
        var width = payload.substr(0, i);
        var height = payload.substr(i+1);
        var lwidth = listener_.clientWidth;
        var lheight = listener_.clientHeight;
        var marginleft = (lwidth-width)/2;
        var margintop = (lheight-height)/2;
        CriatModule.style.marginLeft = (marginleft > 0 ? marginleft : 0) + "px";
        CriatModule.style.marginTop = (margintop > 0 ? margintop : 0) + "px";
        CriatModule.width = width;
        CriatModule.height = height;
    }
}

function handleResize() {
    console.log("resize! " + listener_.clientWidth + "/" + listener_.clientHeight);
    if (CriatModule)
        CriatModule.postMessage('resize:' + listener_.clientWidth + "/" + listener_.clientHeight);
}

document.addEventListener('DOMContentLoaded', function() {
    listener_ = document.getElementById('listener');
    listener_.addEventListener('load', moduleDidLoad, true);
    listener_.addEventListener('message', handleMessage, true);
    window.addEventListener('resize', handleResize, true);
})
