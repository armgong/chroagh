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
#include "ppapi/utility/completion_callback_factory.h"
#include "ppapi/cpp/host_resolver.h"
#include "ppapi/cpp/tcp_socket.h"

namespace {

    // The expected string sent by the browser.
    const char* const kHelloString = "hello";
    // The string sent back to the browser upon receipt of a message
    // containing "hello".
    const char* const kReplyString = "hello from NaCl";

    const int debug = 0;
}  // namespace

class CriatInstance : public pp::Instance {
public:
    explicit CriatInstance(PP_Instance instance)
        : pp::Instance(instance),
          callback_factory_(this),
          image_data_(NULL),
          k_(0),
          socket_(this),
          connected_(false),
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

        if (!pp::HostResolver::IsAvailable()) {
            LogMessage(0, "HostResolver not available");
            return false;
        }

        resolver_ = pp::HostResolver(this);
        if (resolver_.is_null()) {
            LogMessage(0, "Error creating HostResolver.");
            return false;
        }

        PP_HostResolver_Hint hint = { PP_NETADDRESS_FAMILY_UNSPECIFIED, 0 };
        resolver_.Resolve("127.0.0.1", 30002, hint,
        callback_factory_.NewCallback(&CriatInstance::OnResolveCompletion));
        
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
    
    virtual bool HandleInputEvent(const pp::InputEvent& event) {
#if 0
        if (event.GetType() == PP_INPUTEVENT_TYPE_MOUSEDOWN ||
            event.GetType() == PP_INPUTEVENT_TYPE_MOUSEMOVE) {
            pp::MouseInputEvent mouse_event(event);
            
            if (mouse_event.GetButton() == PP_INPUTEVENT_MOUSEBUTTON_NONE)
                return true;
            
            mouse_ = mouse_event.GetPosition();
            mouse_down_ = true;
        }
        
        if (event.GetType() == PP_INPUTEVENT_TYPE_MOUSEUP)
            mouse_down_ = false;
#endif
        
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

    void OnResolveCompletion(int32_t result) {
        if (result != PP_OK) {
            if (result == PP_ERROR_NOACCESS)
                LogMessage(0, "No access.");
            LogMessage(0, "Resolve failed.");
            return;
        }
        
        pp::NetAddress addr = resolver_.GetNetAddress(0);
        LogMessage(1, std::string("Resolved: ") +
                    addr.DescribeAsString(true).AsString());
        
        LogMessage(1, "Connecting ...");
        socket_.Connect(addr,
                        callback_factory_.NewCallback(&CriatInstance::OnConnectCompletion));
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

        LogMessage(1, "Connected");
        connected_ = true;
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
        image_data_ = new pp::ImageData(this, format, size_, true);
        image_pos_ = 0;
    }

    void TCPSend(char cmd) {
        std::stringstream status;
        status << "TCPSend: " << cmd;
        LogMessage(5, status.str());

        send_buffer[0] = cmd;
        socket_.Write(send_buffer, 8,
            callback_factory_.NewCallback(&CriatInstance::OnWriteCompletion)); 
   }

    void OnWriteCompletion(int32_t result) {
        std::stringstream status;
        status << "WriteCompletion: " << result;
        LogMessage(5, status.str());
    }

    void FillBuffer() {
        char* data = static_cast<char*>(image_data_->data());
        std::stringstream status;
        status << "FillBuffer: " << (long)data;
        LogMessage(5, status.str());

        uint32_t totalsize = size_.width() * size_.height() * 4;
        size_t length = totalsize-image_pos_;
        if (connected_ && length > 0)
            socket_.Read(data+image_pos_, length,
                         callback_factory_.NewCallback(&CriatInstance::OnReadCompletion));
        else {
            screen_flying_ = false;
            if (connected_) {
                screen_flying_ = true;
                /* Ask for the next frame already */
                TCPSend('S');
            }
            OnFrameReady(0);
        }
    }

    void OnReadCompletion(int32_t result) {
        std::stringstream status;
        status << "ReadCompletion: " << result << ". (" << image_pos_ << ")";
        LogMessage(5, status.str());

        if (result < 0) {
            connected_ = false;
            FillBuffer();
            return;
        }

        image_pos_ += result;
        FillBuffer();
    }

    void OnFlush(int32_t) {
        LogMessage(5, "OnFlush");

        AllocateImage(false);
        if (connected_) {
            if (!screen_flying_) {
                screen_flying_ = true;
                TCPSend('S');
            }
            FillBuffer();
        } else {
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

    pp::HostResolver resolver_;
    pp::TCPSocket socket_;
    bool connected_;
    bool screen_flying_;

    PP_Time lasttime_;
    double avgfps_;

    char send_buffer[8];
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
