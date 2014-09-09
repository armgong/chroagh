var URL = "ws://localhost:30001/";
var VERSION = 1; /* Note: the extension must always be backward compatible */

/* Global variables */
var canvas_ = null;
var websocket_ = null; /* Active connection */
var active_ = false;
var status_ = "";

var context_;
var image_;
var imageindex_ = 0;

function init() {
    canvas_ = document.getElementById('canvas');
    websocketConnect();
    //requestAnimationFrame(display);

    context_ = canvas_.getContext('2d');

    image_ = context_.createImageData(canvas.width, canvas.height);
}

/* TODO: code reuse with extension? */
/* Connect to the server */
function websocketConnect() {
    console.log("websocketConnect: " + websocket_);

/*    websocket_ = new WebSocket(URL);
    websocket_.binaryType = "arraybuffer";
    websocket_.onopen = websocketOpen;
    websocket_.onmessage = websocketMessage;
    websocket_.onclose = websocketClose;*/

    chrome.sockets.tcp.create({}, function(createInfo) {
        tcpsocket_ = createInfo.socketId;
        chrome.sockets.tcp.connect(createInfo.socketId,
                                   "127.0.0.1", 30002, onConnectedCallback);
    });
}

function onConnectedCallback(result) {
    console.log("connected" + result);
    chrome.sockets.tcp.onReceive.addListener(function(info) {
        if (info.socketId != tcpsocket_)
            return;
        // info.data is an arrayBuffer.
        var i8 = new Uint8Array(info.data);
        //console.log("data " + i8.length + "/" + imageindex_);
        for (var i = 0; i < i8.length; i++) {
            image_.data[imageindex_+i] = i8[i];
            if (imageindex_+i+1 == image_.data.length) {
                requestAnimationFrame(display);
                imageindex_ = 0;
                /* Should stop getting data here, until it's up */
                chrome.sockets.tcp.setPaused(tcpsocket_, true);
            }
        }
        imageindex_ += i8.length;
    });
}

function websocketOpen() {
    console.log("Connection established");
}

var screen_ = false;
var data_ = null;

function websocketMessage(evt) {
    if (active_ && screen_) {
        screen_ = false;
        if (data_ == null)
            data_ = evt.data;
        //console.log("Got data!");
        requestAnimationFrame(display);
        return;
    }
    console.log("Should not reach here");

    var received_msg = evt.data;

    var cmd = received_msg[0];
    var payload = received_msg.substring(1);

    /* Only accept version packets until we have received one. */
    if (!active_) {
        if (cmd == 'V') { /* Version */
            if (payload < 1 || payload > VERSION) {
                websocket_.send("EInvalid version (> " + VERSION + ")");
                error("Invalid server version " + payload + " > " + VERSION,
                      false);
            }
            active_ = true;
            websocket_.send("VOK");
            websocket_.send("S"); /* Ask for a frame */
            screen_ = true;
            requestAnimationFrame(display);
            return;
        } else {
            error("Received frame while waiting for version", false);
        }
    }   
}

function websocketClose() {
    if (websocket_ == null) {
        console.log("websocketClose: null!");
        return;
    }

    console.log("Connection closed");

    websocket_ = null;
}

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
function display(timestamp) {
    console.log("t=" + 1000/(timestamp-lastt));
    lastt = timestamp;

    //var i8 = new Uint8Array(data_);
    //image.data = i8;
    //for (var i = 0; i < image_.data.length; i++) {
    //    image_.data[i] = i8[i];
    //}
    context_.putImageData(image_, 0, 0);

    k++;

    if (k < 1000) {
        chrome.sockets.tcp.setPaused(tcpsocket_, false);
        //websocket_.send("S"); /* Ask for a frame */
        //screen_ = true;
        //requestAnimationFrame(display);
    }
}

document.addEventListener('DOMContentLoaded', function() {
    init();
});

