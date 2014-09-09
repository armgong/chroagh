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

function tcpsend(cmd) {
    var buf = new ArrayBuffer(1);
    var bufView = new Uint8Array(buf);
    bufView[0] = "a".charCodeAt(0);

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

    if (k < 10000) {
        chrome.sockets.tcp.setPaused(tcpsocket_, false);
        tcpsend("S");
        //websocket_.send("S"); /* Ask for a frame */
        //screen_ = true;
        //requestAnimationFrame(display);
    }
}

document.addEventListener('DOMContentLoaded', function() {
    init();
});

