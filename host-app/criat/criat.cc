// Copyright (c) 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <stdio.h>
#include <stdlib.h>
#include <sstream>

#include "ppapi/c/ppb_image_data.h"
#include "ppapi/cpp/graphics_2d.h"
#include "ppapi/cpp/image_data.h"
#include "ppapi/cpp/input_event.h"
#include "ppapi/cpp/instance.h"
#include "ppapi/cpp/module.h"
#include "ppapi/cpp/var.h"
#include "ppapi/cpp/point.h"
#include "ppapi/utility/completion_callback_factory.h"

namespace {

// The expected string sent by the browser.
const char* const kHelloString = "hello";
// The string sent back to the browser upon receipt of a message
// containing "hello".
const char* const kReplyString = "hello from NaCl";

}  // namespace

class CriatInstance : public pp::Instance {
public:
    explicit CriatInstance(PP_Instance instance)
        : pp::Instance(instance),
          callback_factory_(this),
          image_data_(NULL),
          k_(0),
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
            pp::Var var_reply(kReplyString);
            PostMessage(var_reply);
        }
    }
    
    virtual bool Init(uint32_t argc, const char* argn[], const char* argv[]) {
        RequestInputEvents(PP_INPUTEVENT_CLASS_MOUSE);
        
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
            MainLoop(0);
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
    bool CreateContext(const pp::Size& new_size) {
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
    
    void Paint() {
        uint32_t* data = static_cast<uint32_t*>(image_data_->data());
        if (!data)
            return;

        //std::stringstream ss;
        //ss << "Paint " << data;
        //PostMessage(ss.str());
        
        k_ += 1;

        uint32_t num_pixels = size_.width() * size_.height();
        size_t offset = 0;
        for (uint32_t i = 0; i < num_pixels; ++i) {
            data[offset] = 0xff00ff00 + (k_ << 16) + i;
            offset++;
        }
        
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
        image_data_->detach();
    }
    
    void MainLoop(int32_t t) {
        PP_Time time_ = pp::Module::Get()->core()->GetTime();
        double cfps = 1.0/(time_-lasttime_);
        lasttime_ = time_;

        avgfps_ = 0.9*avgfps_ + 0.1*cfps;
        if ((k_ % 60) == 0) {
            std::stringstream ss;
            ss << "fps: " << (int)(cfps+0.5) << " (" << (int)(avgfps_+0.5) << ")";
            PostMessage(ss.str());
        }
        
        if (context_.is_null()) {
            // The current Graphics2D context is null, so updating and rendering is
            // pointless. Set flush_context_ to null as well, so if we get another
            // DidChangeView call, the main loop is started again.
            flush_context_ = context_;
            return;
        }

        const bool kDontInitToZero = false;
        PP_ImageDataFormat format = pp::ImageData::GetNativeImageDataFormat();
        image_data_ = new pp::ImageData(this, format, size_, kDontInitToZero);
        
        Paint();
        // Store a reference to the context that is being flushed; this ensures
        // the callback is called, even if context_ changes before the flush
        // completes.
        flush_context_ = context_;
        context_.Flush(
            callback_factory_.NewCallback(&CriatInstance::MainLoop));
    }

    pp::CompletionCallbackFactory<CriatInstance> callback_factory_;
    pp::Graphics2D context_;
    pp::Graphics2D flush_context_;
    pp::Size size_;

    pp::ImageData* image_data_;
    int k_;

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
