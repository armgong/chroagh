// Copyright (c) 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <stdio.h>
#include <stdlib.h>
#include <sstream>
#include <GLES2/gl2.h>

#include "ppapi/cpp/net_address.h"
#include "ppapi/c/ppb_tcp_socket.h"
#include "ppapi/c/ppb_image_data.h"
#include "ppapi/cpp/graphics_3d.h"
#include "ppapi/cpp/image_data.h"
#include "ppapi/cpp/input_event.h"
#include "ppapi/cpp/instance.h"
#include "ppapi/cpp/module.h"
#include "ppapi/cpp/var.h"
#include "ppapi/cpp/point.h"
#include "ppapi/utility/completion_callback_factory.h"
#include "ppapi/cpp/var.h"
#include "ppapi/cpp/var_array_buffer.h"
#include "ppapi/cpp/websocket.h"
#include "ppapi/lib/gl/gles2/gl2ext_ppapi.h"

namespace {
    // The expected string sent by the browser.
    const char* const kHelloString = "hello";
    // The string sent back to the browser upon receipt of a message
    // containing "hello".
    const char* const kReplyString = "hello from NaCl";

    const int debug = 1;

GLuint CompileShader(GLenum type, const char* data) {
  GLuint shader = glCreateShader(type);
  glShaderSource(shader, 1, &data, NULL);
  glCompileShader(shader);

  GLint compile_status;
  glGetShaderiv(shader, GL_COMPILE_STATUS, &compile_status);
  if (compile_status != GL_TRUE) {
    // Shader failed to compile, let's see what the error is.
    char buffer[1024];
    GLsizei length;
    glGetShaderInfoLog(shader, sizeof(buffer), &length, &buffer[0]);
    fprintf(stderr, "Shader failed to compile: %s\n", buffer);
    return 0;
  }

  return shader;
}

GLuint LinkProgram(GLuint frag_shader, GLuint vert_shader) {
  GLuint program = glCreateProgram();
  glAttachShader(program, frag_shader);
  glAttachShader(program, vert_shader);
  glLinkProgram(program);

  GLint link_status;
  glGetProgramiv(program, GL_LINK_STATUS, &link_status);
  if (link_status != GL_TRUE) {
    // Program failed to link, let's see what the error is.
    char buffer[1024];
    GLsizei length;
    glGetProgramInfoLog(program, sizeof(buffer), &length, &buffer[0]);
    fprintf(stderr, "Program failed to link: %s\n", buffer);
    return 0;
  }

  return program;
}

const char kFragShaderSource[] =
    "precision mediump float;\n"
    "varying vec2 v_texcoord;\n"
    "uniform sampler2D u_texture;\n"
    "void main() {\n"
    "  gl_FragColor = texture2D(u_texture, v_texcoord);\n"
    "}\n";

const char kVertexShaderSource[] =
    "attribute vec2 a_texcoord;\n"
    "attribute vec4 a_position;\n"
    "varying vec2 v_texcoord;\n"
    "void main() {\n"
    "  gl_Position = a_position;\n"
    "  v_texcoord = a_texcoord;\n"
    "}\n";

    static const float kVerts[]  = {-1, -1, 0, -1,  1, 0, 1, 1, 0,
                                    -1, -1, 0,  1, -1, 0, 1, 1, 0};
    static const float kTextCoords[] = { 0, 0, 0, 1, 1, 1,
                                         0, 0, 1, 0, 1, 1};

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
        bool wasnull = false;
        
        LogMessage(5, "CreateContext");

        if (context_.is_null()) {
            wasnull = true;
            if (!InitGL(new_size.width(), new_size.height())) {
                // failed.
                return;
            }

            InitShaders();
        } else {
            // Resize the buffers to the new size of the module.
            int32_t result = context_.ResizeBuffers(new_size.width(), new_size.height());
            if (result < 0) {
                fprintf(stderr,
                        "Unable to resize buffers to %d x %d!\n",
                        new_size.width(), new_size.height());
                return;
            }
        }

        size_ = new_size;
        /* TODO: Free existing image_data_ */
        image_data_ = new unsigned char[size_.width()*size_.height()*4];
        glViewport(0, 0, size_.width(), size_.height());

        if (wasnull)
            OnFlush(0);

        return;
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

            pp::VarArrayBuffer array_buffer(4);
            char* data = static_cast<char*>(array_buffer.Map());
            data[0] = 'K';
            data[1] = event.GetType() == PP_INPUTEVENT_TYPE_KEYDOWN ? 1 : 0;
            *(uint16_t*)(data+2) = keysym;
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

                pp::VarArrayBuffer array_buffer(3);
                char* data = static_cast<char*>(array_buffer.Map());
                data[0] = 'C';
                data[1] = event.GetType() == PP_INPUTEVENT_TYPE_MOUSEDOWN ? 1 : 0;
                data[2] = mouse_event.GetButton()+1;
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
                /* FIXME: Check payload */
                screen_flying_ = false;
                OnFrameReady(0);
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
            pp::VarArrayBuffer array_buffer(5);
            char* data = static_cast<char*>(array_buffer.Map());
            data[0] = 'M';
            *(uint16_t*)(data+1) = mouse_pos_.x();
            *(uint16_t*)(data+3) = mouse_pos_.y();
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

    void AllocateImage(bool initzero) {
        LogMessage(5, "AllocateImage");
        /* FIXME: No-op? */
        image_pos_ = 0;
    }

    void RequestScreen() {
        if (screen_flying_) {
            return;
        }

        screen_flying_ = true;

        pp::VarArrayBuffer array_buffer(8*3);
        char* send_buffer = static_cast<char*>(array_buffer.Map());
        send_buffer[0] = 'S';
        send_buffer[1] = 1; /* shm */
        *(uint16_t*)(send_buffer+2) = size_.width();
        *(uint16_t*)(send_buffer+4) = size_.height();

        uint64_t* data = static_cast<uint64_t*>((void*)image_data_);
        uint64_t rnd = rand();
        rnd = (rnd << 32) ^ rand();
        *data = rnd;

        *(uint64_t*)(send_buffer+8) = (uint64_t)image_data_;
        *(uint64_t*)(send_buffer+16) = rnd;

        array_buffer.Unmap();
        SocketSend(array_buffer);
    }

    void FillBuffer() {
        char* data = (char*)(image_data_);
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

    void OnFlush(int32_t) {
        LogMessage(5, "OnFlush");

        AllocateImage(false);
        if (connected_) {
            RequestScreen();
            FillBuffer();
        } else {
            std::stringstream status;
            status << "OnFlush: " << (long)image_data_;
            LogMessage(5, status.str());

            if (k_ < 5) {
                uint32_t* data = (uint32_t*)image_data_;
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

  bool InitGL(int32_t new_width, int32_t new_height) {
    if (!glInitializePPAPI(pp::Module::Get()->get_browser_interface())) {
      fprintf(stderr, "Unable to initialize GL PPAPI!\n");
      return false;
    }

    const int32_t attrib_list[] = {
      PP_GRAPHICS3DATTRIB_ALPHA_SIZE, 8,
      PP_GRAPHICS3DATTRIB_DEPTH_SIZE, 24,
      PP_GRAPHICS3DATTRIB_WIDTH, new_width,
      PP_GRAPHICS3DATTRIB_HEIGHT, new_height,
      PP_GRAPHICS3DATTRIB_NONE
    };

    context_ = pp::Graphics3D(this, attrib_list);
    if (!BindGraphics(context_)) {
      fprintf(stderr, "Unable to bind 3d context!\n");
      context_ = pp::Graphics3D();
      glSetCurrentContextPPAPI(0);
      return false;
    }

    glSetCurrentContextPPAPI(context_.pp_resource());
    return true;
  }

  void InitShaders() {
    frag_shader_ = CompileShader(GL_FRAGMENT_SHADER, kFragShaderSource);
    if (!frag_shader_)
      return;

    vertex_shader_ = CompileShader(GL_VERTEX_SHADER, kVertexShaderSource);
    if (!vertex_shader_)
      return;

    program_ = LinkProgram(frag_shader_, vertex_shader_);
    if (!program_)
      return;

    texture_loc_ = glGetUniformLocation(program_, "u_texture");
    position_loc_ = glGetAttribLocation(program_, "a_position");
    texcoord_loc_ = glGetAttribLocation(program_, "a_texcoord");
  }

  void InitTexture() {
      glGenTextures(1, &texture_);
      glBindTexture(GL_TEXTURE_2D, texture_);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
      glTexImage2D(GL_TEXTURE_2D,
                   0,
                   GL_RGBA,
                   size_.width(),
                   size_.height(),
                   0,
                   GL_RGBA,
                   GL_UNSIGNED_BYTE,
                   &image_data_[0]);
  }

  void Render() {
    InitTexture();

//    glClearColor(0.5, 0.5, 0.5, 1);
//    glClearDepthf(1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    //set what program to use
    glUseProgram(program_);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texture_);
    glUniform1i(texture_loc_, 0);

    glVertexAttribPointer(position_loc_,
                          3,
                          GL_FLOAT,
                          GL_FALSE,
                          0,
                          kVerts);
    glEnableVertexAttribArray(position_loc_);
    glVertexAttribPointer(texcoord_loc_,
                          2,
                          GL_FLOAT,
                          GL_FALSE,
                          0,
                          kTextCoords);
    glEnableVertexAttribArray(texcoord_loc_);

    glDrawArrays(GL_TRIANGLES, 0, 6);
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
            return;
        }

        //if (k_ > 100) return;
        //Render();
        context_.SwapBuffers(
            callback_factory_.NewCallback(&CriatInstance::OnFlush));
    }

    pp::CompletionCallbackFactory<CriatInstance> callback_factory_;
    pp::Graphics3D context_;
    pp::Size size_;
    GLuint frag_shader_;
    GLuint vertex_shader_;
    GLuint program_;
    GLuint texture_;

    GLuint texture_loc_;
    GLuint position_loc_;
    GLuint texcoord_loc_;

    unsigned char* image_data_;
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
