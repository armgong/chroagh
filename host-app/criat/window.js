var URL = "ws://localhost:30001/";
var VERSION = 1; /* Note: the extension must always be backward compatible */

/* Global variables */
var canvas_ = null;
var websocket_ = null; /* Active connection */
var active_ = false;
var status_ = "";

var context_;
var image_;
var image_back_;
var imageindex_ = 0;

function init() {
    canvas_ = document.getElementById('canvas');
    websocketConnect();
    //requestAnimationFrame(display);

    context_ = canvas_.getContext('2d');

    image_ = context_.createImageData(canvas.width, canvas.height);
    image_back_ = context_.createImageData(canvas.width, canvas.height);

    document.addEventListener("keydown", onKey);
    document.addEventListener("keyup", onKey);
    document.addEventListener("keypress", onKeyPress);
    canvas_.addEventListener("mousedown", onMouse);
    canvas_.addEventListener("mouseup", onMouse);
    canvas_.addEventListener("mousemove", onMouse);
    canvas_.addEventListener('contextmenu', function(ev) {
        ev.preventDefault();
        return false;
    });
    /* TODO: handle mouse exit (release button) */
    /* TODO: mousewheel */
    /* TODO: touchscreen */
}

/* TODO: code reuse with extension? */
/* Connect to the server */
function websocketConnect() {
    console.log("websocketConnect: " + websocket_);

    chrome.sockets.tcp.create({ "bufferSize": 32768 }, function(createInfo) {
        tcpsocket_ = createInfo.socketId;
        chrome.sockets.tcp.connect(createInfo.socketId,
                                   "127.0.0.1", 30002, onConnectedCallback);
    });
}

function tcpsend(cmd, param) {
    var buf = new ArrayBuffer(8);
    var bufView = new Uint8Array(buf);
    bufView[0] = cmd.charCodeAt(0);
    for (var i = 1; i < bufView.length; i++) {
        bufView[i] = (param && param.length > i-1) ? param[i-1] : 0;
    }

    chrome.sockets.tcp.send(tcpsocket_,
        buf,
        function(resultCode) {
            //console.log("Data sent to new TCP client connection.")
        });
}

function onConnectedCallback(result) {
    console.log("connected" + result);

    tcpsend("S");

    chrome.sockets.tcp.onReceive.addListener(function(info) {
        if (info.socketId != tcpsocket_)
            return;
        // info.data is an arrayBuffer.
        var i8 = new Uint8Array(info.data);
        //console.log("data " + i8.length + "/" + imageindex_);
        for (var i = 0; i < i8.length; i++, imageindex_++) {
            image_back_.data[imageindex_] = i8[i];
            if (imageindex_+1 >= image_.data.length) {
                requestAnimationFrame(display);
                imageindex_ = -1;
                tmp = image_back_;
                image_back_ = image_;
                image_ = tmp;
                chrome.sockets.tcp.setPaused(tcpsocket_, true);
            }
        }
    });
}

var screen_ = false;
var data_ = null;

/* Set the current status string.
 * active is a boolean, true if the WebSocket connection is established. */
function setStatus(status, active) {
    active_ = active;
    status_ = status;
}

/* Display an error, and prevent retries if enabled is false */
function error(str, enabled) {
    websocket_.close();
}

/* Display code */

var k = 0;
var lastt = 0;
var avgfps = 0;
function display(timestamp) {
    //console.log("t=" + 1000/(timestamp-lastt));
    fps = document.getElementById("fps");
    cfps = 1000/(timestamp-lastt);
    avgfps = 0.9*avgfps + 0.1*cfps;
    fps.textContent = "fps:" + Math.round(cfps) + " (" + Math.round(avgfps) + ")";

    lastt = timestamp;

    //var i8 = new Uint8Array(data_);
    //image.data = i8;
    //for (var i = 0; i < image_.data.length; i++) {
    //    image_.data[i] = i8[i];
    //}
    context_.putImageData(image_, 0, 0);

    k++;

    if (k < 10*60*60) {
        chrome.sockets.tcp.setPaused(tcpsocket_, false);
        tcpsend("S");
        //websocket_.send("S"); /* Ask for a frame */
        //screen_ = true;
        //requestAnimationFrame(display);
    } else {
        fps.textContent = "stalled";
    }
}

/* Input code */

// See http://unixpapa.com/js/key.html
function keyCodeToKeysym(code) {
    if (code >= 65 && code <= 90) { /* A to Z */
        return code+32;
    }

    if (code >= 48 && code <= 57) { /* 0 to 9 */
        return code;
    }

    if (code >= 96 && code <= 105) { /* KP 0 to 9 */
        return code-96+0xffb0;
    }

    if (code >= 112 && code <= 123) { /* F1-F12 */
        return code-112+0xffbe;
    }

    switch(code) {
    case 8: return 0xff08;
    case 9: return 0xff09;
    case 12: return 0xff9d; // num 5
    case 13: return 0xff0d;
    case 16: return 0xffe1; // shift
    case 17: return 0xffe3; // control
    case 18: return 0xffe9; // alt
    case 19: return 0xff13; // pause
    case 20: return 0xffe5;
    case 27: return 0xff1b;
    case 32: return 0x20; // space
    case 33: return 0xff55; // page up
    case 34: return 0xff56; // page down
    case 35: return 0xff57; // end
    case 36: return 0xff50; // home
    case 37: return 0xff51; // left
    case 38: return 0xff52; // top
    case 39: return 0xff53; // right
    case 40: return 0xff54; // bottom
    case 42: return 0xff61; // print screen
    case 45: return 0xff63; // insert
    case 46: return 0xffff; // delete
    case 91: return 0xffeb; // super
    case 106: return 0xffaa; // num multiply
    case 107: return 0xffab; // num plus
    case 109: return 0xffad; // num minus
    case 110: return 0xffae; // num dot
    case 111: return 0xffaf; // num divide
    case 144: return 0xff7f; // num lock (maybe better not to pass through???)
    case 145: return 0xff14; // scroll lock
    case 186: return 0x3b;
    case 187: return 0x3d;
    case 188: return 0x2c;
    case 189: return 0x2d;
    case 190: return 0x2e;
    case 191: return 0x2f;
    case 192: return 0x60;
    case 219: return 0x5b;
    case 220: return 0x5c;
    case 221: return 0x5d;
    case 222: return 0x27;
    }

    return null;
}

function onKey(e) {
    sym = keyCodeToKeysym(e.keyCode);
    //console.log(e.type + " " + e.keyCode + " =>" + sym);
    if (sym)
        tcpsend("K", [ e.type == "keydown" ? 1 : 0, sym >> 8, sym ]);
}

/* Can we do something smart with this? */
function onKeyPress(e) {
//    console.log("press " + e.keyCode);
//    tcpsend("K", [ e.keyCode, 0 ]);
}

/* FIXME: To prevent lag, do not send new M events until acknowledged */
var e_;
function onMouse(e) {
    e_ = e;
    e.preventDefault();

    //console.log(e.type + " " + e.layerX + "x" + e.layerY);
    tcpsend("M", [ e.layerX >> 8, e.layerX,
                   e.layerY >> 8, e.layerY ]);

    if (e.type != "mousemove") {
        //console.log("/" + e.which);
        tcpsend("C", [ e.type == "mousedown" ? 1 : 0, e.which ]);
    }
    return false;
}

document.addEventListener('DOMContentLoaded', function() {
    init();
});

