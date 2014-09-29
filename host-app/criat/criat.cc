// Copyright (c) 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <stdio.h>
#include <stdlib.h>
#include <sstream>

#include "ppapi/cpp/net_address.h"
#include "ppapi/c/ppb_tcp_socket.h"
#include "ppapi/c/ppb_image_data.h"
#include "ppapi/cpp/graphics_2d.h"
#include "ppapi/cpp/image_data.h"
#include "ppapi/cpp/input_event.h"
#include "ppapi/cpp/instance.h"
#include "ppapi/cpp/module.h"
#include "ppapi/cpp/var.h"
#include "ppapi/cpp/point.h"
#include "ppapi/cpp/message_loop.h"
#include "ppapi/utility/completion_callback_factory.h"
#include "ppapi/cpp/var.h"
#include "ppapi/cpp/var_array_buffer.h"
#include "ppapi/cpp/websocket.h"

namespace {

    // The expected string sent by the browser.
    const char* const kHelloString = "hello";
    // The string sent back to the browser upon receipt of a message
    // containing "hello".
    const char* const kReplyString = "hello from NaCl";

#include "../../src/fbserver-proto.h"

    const int debug = 1;
}  // namespace

class CriatInstance : public pp::Instance {
public:
    explicit CriatInstance(PP_Instance instance)
        : pp::Instance(instance),
          callback_factory_(this),
          image_data_(NULL),
          k_(0),
          websocket_(NULL),
          connected_(false),
          pending_mouse_move_(false),
          avgfps_(0) {}
    
    virtual ~CriatInstance() { }
    
    virtual void HandleMessage(const pp::Var& var_message) {
        // Ignore the message if it is not a string.
        if (!var_message.is_string())
            return;
        
        // Get the string message and compare it to "hello".
        std::string message = var_message.AsString();
        if (message == kHelloString) {
            // If it matches, send our response back to JavaScript.
            LogMessage(0, kReplyString);
        }
    }
    
    virtual bool Init(uint32_t argc, const char* argn[], const char* argv[]) {
        RequestInputEvents(PP_INPUTEVENT_CLASS_MOUSE);
        RequestFilteringInputEvents(PP_INPUTEVENT_CLASS_KEYBOARD);

        srand(pp::Module::Get()->core()->GetTime());

        websocket_ = new pp::WebSocket(this);
        if (!websocket_)
            return false;
        websocket_->Connect(pp::Var("ws://localhost:30010/"), NULL, 0,
                            callback_factory_.NewCallback(&CriatInstance::OnConnectCompletion));
        PostMessage(pp::Var("connecting..."));

        return true;
    }
    
    virtual void DidChangeView(const pp::View& view) {
        pp::Size new_size = view.GetRect().size();
        
        if (!CreateContext(new_size))
            return;
        
        // When flush_context_ is null, it means there is no Flush callback in
        // flight. This may have happened if the context was not created
        // successfully, or if this is the first call to DidChangeView (when the
        // module first starts). In either case, start the main loop.
        if (flush_context_.is_null())
            OnFlush(0);
    }

    // See http://unixpapa.com/js/key.html
    // We might want to use an array instead...
    uint16_t KeyCodeToKeySym(uint32_t code) {
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

        return 0x00;
    }

    virtual bool HandleInputEvent(const pp::InputEvent& event) {
        if (event.GetType() == PP_INPUTEVENT_TYPE_KEYDOWN ||
            event.GetType() == PP_INPUTEVENT_TYPE_KEYUP) {
            pp::KeyboardInputEvent key_event(event);

            uint32_t keycode = key_event.GetKeyCode();
            uint16_t keysym = KeyCodeToKeySym(keycode);

            std::ostringstream status;
            status << "Key " << (event.GetType() == PP_INPUTEVENT_TYPE_KEYDOWN ? "DOWN" : "UP");
            status << ": " << std::hex << keycode;
            status << " @ " << std::hex << keysym;
            LogMessage(1, status.str());

            struct key* k;
            pp::VarArrayBuffer array_buffer(sizeof(*k));
            k = static_cast<struct key*>(array_buffer.Map());
            k->type = 'K';
            k->down = event.GetType() == PP_INPUTEVENT_TYPE_KEYDOWN ? 1 : 0;
            k->keysym = keysym;
            array_buffer.Unmap();
            SocketSend(array_buffer);
            return false;
        }

        if (event.GetType() == PP_INPUTEVENT_TYPE_MOUSEDOWN ||
            event.GetType() == PP_INPUTEVENT_TYPE_MOUSEUP ||
            event.GetType() == PP_INPUTEVENT_TYPE_MOUSEMOVE) {
            pp::MouseInputEvent mouse_event(event);

            pending_mouse_move_ = true;
            mouse_pos_ = mouse_event.GetPosition();

            std::ostringstream status;
            status << "Mouse " << mouse_event.GetPosition().x() << "x" << mouse_event.GetPosition().y();

            if (event.GetType() != PP_INPUTEVENT_TYPE_MOUSEMOVE) {
                status << " " << (event.GetType() == PP_INPUTEVENT_TYPE_MOUSEDOWN ? "DOWN" : "UP");
                status << " " << (mouse_event.GetButton());

                struct mouseclick* mc;
                pp::VarArrayBuffer array_buffer(sizeof(*mc));
                mc = static_cast<struct mouseclick*>(array_buffer.Map());
                mc->type = 'C';
                mc->down = event.GetType() == PP_INPUTEVENT_TYPE_MOUSEDOWN ? 1 : 0;
                mc->button = mouse_event.GetButton()+1;
                array_buffer.Unmap();
                SocketSend(array_buffer);
            }

            LogMessage(3, status.str());
        }
        
        return true;
    }
    
private:
    void LogMessage(int level, std::string str) {
        double delta = (pp::Module::Get()->core()->GetTime()-lasttime_)*1000;

        if (level <= debug) {
            std::ostringstream status;
            status << (int)delta << " " << str;
            PostMessage(status.str());
        }
    }

    void OnConnectCompletion(int32_t result) {
        if (result != PP_OK) {
            if (result == PP_ERROR_NOACCESS)
                LogMessage(0, "No access.");

            std::ostringstream status;
            status << "Connection failed: " << result;
            LogMessage(0, status.str());
            return;
        }

        /* FIXME: I think this can complete synchronously?!?! */
        SocketReceive();

        LogMessage(1, "Connected");
    }

    void OnReceiveCompletion(int32_t result) {
        std::stringstream status;
        status << "ReadCompletion: " << result << ". (" << image_pos_ << ")";
        LogMessage(3, status.str());

        if (result == PP_OK) {
            const char* data;
            if (receive_var_.is_array_buffer()) {
                pp::VarArrayBuffer array_buffer(receive_var_);
                LogMessage(3, "receive (binary)");
                data = static_cast<char*>(array_buffer.Map());
            }
            else {
                LogMessage(3, "receive (text): " + receive_var_.AsString());
                data = receive_var_.AsString().c_str();
            }

            if (data[0] == 'V') {
                if (connected_) {
                    LogMessage(0, "Got a version while connected?!?");
                }
                /* FIXME: Check version */
                websocket_->SendMessage(pp::Var("VOK"));
                connected_ = true;
                return;
            } else if (data[0] == 'S') {
                struct screen_reply* reply = (struct screen_reply*)data;
                screen_flying_ = false;
                if (reply->updated) {
                    OnFrameReady(0);
                } else {
                    /* TODO: Delay! */
                    pp::MessageLoop::GetForMainThread().PostWork(
                        callback_factory_.NewCallback(&CriatInstance::OnWaitEnd),
                        15);
                }
                return;
            } else {
                std::stringstream status;
                status << "Error: first char " << (int)data[0];
                LogMessage(0, status.str());
            }
        }

        LogMessage(0, "Receive error.");
        // TODO : Not ok, so what? Disconnect?
        connected_ = false;
        FillBuffer();
    }

    void SocketSend(const pp::Var& var) {
        /* TODO: Send pending mouse move */

        if (pending_mouse_move_) {
            struct mousemove* mm;
            pp::VarArrayBuffer array_buffer(sizeof(*mm));
            mm = static_cast<struct mousemove*>(array_buffer.Map());
            mm->type = 'M';
            mm->x = mouse_pos_.x();
            mm->y = mouse_pos_.y();
            array_buffer.Unmap();
            websocket_->SendMessage(array_buffer);
            pending_mouse_move_ = false;
        }

        websocket_->SendMessage(var);
    }

    void SocketReceive() {
        websocket_->ReceiveMessage(&receive_var_, 
                                   callback_factory_.NewCallback(&CriatInstance::OnReceiveCompletion));
    }

    bool CreateContext(const pp::Size& new_size) {
        LogMessage(5, "CreateContext");

        const bool kIsAlwaysOpaque = true;
        context_ = pp::Graphics2D(this, new_size, kIsAlwaysOpaque);
        if (!BindGraphics(context_)) {
            fprintf(stderr, "Unable to bind 2d context!\n");
            context_ = pp::Graphics2D();
            return false;
        }
        
        size_ = new_size;
        
        return true;
    }
    
    void AllocateImage(bool initzero) {
        LogMessage(5, "AllocateImage");
        PP_ImageDataFormat format = pp::ImageData::GetNativeImageDataFormat();
        image_data_ = new pp::ImageData(this, format, size_, initzero);
        image_pos_ = 0;
    }

    void RequestScreen() {
        if (screen_flying_) {
            return;
        }

        screen_flying_ = true;

        struct screen* s;
        pp::VarArrayBuffer array_buffer(sizeof(*s));
        s = static_cast<struct screen*>(array_buffer.Map());

        s->type = 'S';
        s->shm = 1;
        s->width = size_.width();
        s->height = size_.height();
        s->paddr = (uint64_t)image_data_->data();
        uint64_t sig = ((uint64_t)rand() << 32) ^ rand();
        uint64_t* data = static_cast<uint64_t*>(image_data_->data());
        *data = sig;
        s->sig = sig;

        array_buffer.Unmap();
        SocketSend(array_buffer);
    }

    void FillBuffer() {
        char* data = static_cast<char*>(image_data_->data());
        std::stringstream status;
        status << "FillBuffer: " << (long)data;
        LogMessage(5, status.str());

        uint32_t totalsize = size_.width() * size_.height() * 4;
        size_t length = totalsize-image_pos_;
        if (connected_ && length > 0) {
            SocketReceive();
        } else {
            OnFrameReady(0);
        }
    }

    void OnWaitEnd(int32_t) {
        OnFlush(99);
    }

    void OnFlush(int32_t param) {
        LogMessage(5, "OnFlush");

        if (param != 99)
            AllocateImage(false);

        if (connected_) {
            RequestScreen();
            FillBuffer();
        } else {
            if (k_ < 5) {
                uint32_t* data = static_cast<uint32_t*>(image_data_->data());
                uint32_t totalsize = size_.width() * size_.height();
                for (int i = 0; i < totalsize; i++) {
                    data[i] = 0xDEADBEEF;
                }

                std::stringstream status;
                status << "DEADBEEF: " << std::hex << (unsigned long long)data;
                LogMessage(0, status.str());
            }
            /* TODO: Blank image */
            OnFrameReady(0);
        }
    }

    void Paint() {
        /* FIXME: We probably want to switch to PaintImageData on partial
         * updates. */

        // Using Graphics2D::ReplaceContents is the fastest way to update the
        // entire canvas every frame. According to the documentation:
        //
        //   Normally, calling PaintImageData() requires that the browser copy
        //   the pixels out of the image and into the graphics context's backing
        //   store. This function replaces the graphics context's backing store
        //   with the given image, avoiding the copy.
        //
        //   In the case of an animation, you will want to allocate a new image for
        //   the next frame. It is best if you wait until the flush callback has
        //   executed before allocating this bitmap. This gives the browser the
        //   option of caching the previous backing store and handing it back to
        //   you (assuming the sizes match). In the optimal case, this means no
        //   bitmaps are allocated during the animation, and the backing store and
        //   "front buffer" (which the module is painting into) are just being
        //   swapped back and forth.
        //
        context_.ReplaceContents(image_data_);
        //context_.PaintImageData(*image_data_, pp::Point(0, 0));

        /* TODO: I don't think that's correct */
        image_data_->detach();
    }
    
    void OnFrameReady(int32_t) {
        k_++;
        PP_Time time_ = pp::Module::Get()->core()->GetTime();
        double cfps = 1.0/(time_-lasttime_);
        lasttime_ = time_;

        avgfps_ = 0.9*avgfps_ + 0.1*cfps;
        if ((k_ % 60) == 0) {
            std::stringstream ss;
            ss << "fps: " << (int)(cfps+0.5) << " (" << (int)(avgfps_+0.5) << ")";
            LogMessage(0, ss.str());
        }
        
        if (context_.is_null()) {
            // The current Graphics2D context is null, so updating and rendering is
            // pointless. Set flush_context_ to null as well, so if we get another
            // DidChangeView call, the main loop is started again.
            flush_context_ = context_;
            return;
        }

        //if (k_ > 100) return;

        Paint();
        // Store a reference to the context that is being flushed; this ensures
        // the callback is called, even if context_ changes before the flush
        // completes.
        flush_context_ = context_;
        context_.Flush(
            callback_factory_.NewCallback(&CriatInstance::OnFlush));
    }

    pp::CompletionCallbackFactory<CriatInstance> callback_factory_;
    pp::Graphics2D context_;
    pp::Graphics2D flush_context_;
    pp::Size size_;

    pp::ImageData* image_data_;
    size_t image_pos_;
    int k_;

    pp::WebSocket* websocket_;
    bool connected_;
    bool screen_flying_;
    pp::Var receive_var_;

    bool pending_mouse_move_;
    pp::Point mouse_pos_;

    PP_Time lasttime_;
    double avgfps_;
};

class CriatModule : public pp::Module {
public:
    CriatModule() : pp::Module() {}
    virtual ~CriatModule() {}
    
    virtual pp::Instance* CreateInstance(PP_Instance instance) {
        return new CriatInstance(instance);
    }
};

namespace pp {

Module* CreateModule() {
    return new CriatModule();
}

}  // namespace pp
