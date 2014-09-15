// Copyright (c) 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

HelloTutorialModule = null;  // Global application object.
statusText = 'NO-STATUS';

// Indicate load success.
function moduleDidLoad() {
    HelloTutorialModule = document.getElementById('criat');
    updateStatus('SUCCESS');
    // Send a message to the Native Client module
    HelloTutorialModule.postMessage('hello');
}

// If the page loads before the Native Client module loads, then set the
// status message indicating that the module is still loading.  Otherwise,
// do not change the status message.
function pageDidLoad() {
    if (HelloTutorialModule == null) {
        updateStatus('LOADING...');
    } else {
        // It's possible that the Native Client module onload event fired
        // before the page's onload event.  In this case, the status message
        // will reflect 'SUCCESS', but won't be displayed.  This call will
        // display the current message.
        updateStatus();
    }
}

// Set the global status message.  If the element with id 'statusField'
// exists, then set its HTML to the status message as well.
// opt_message The message test.  If this is null or undefined, then
// attempt to set the element with id 'statusField' to the value of
// |statusText|.
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
    var logEl = document.getElementById('log');
    logEl.textContent = message.data;
    //console.log(message.data);
}

document.addEventListener('DOMContentLoaded', function() {
    var listener = document.getElementById('listener');
    listener.addEventListener('load', moduleDidLoad, true);
    listener.addEventListener('message', handleMessage, true);
})
