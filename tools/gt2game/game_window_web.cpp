#include "game_window.h"
#include <SDL.h>
#include <emscripten.h>
#include <emscripten/html5.h>
#include <algorithm>
#include <stdexcept>

extern "C" EMSCRIPTEN_KEEPALIVE void gt2_web_request_close() {
    SDL_Event event{}; event.type=SDL_QUIT; SDL_PushEvent(&event);
}
EM_JS(void, gt2_web_game_exit, (int code), {
    if (Module.onGameExit) Module.onGameExit(code);
});

namespace gt2game {
namespace {
using Clock = std::chrono::steady_clock;
int Key(SDL_Scancode code) {
    if (code>=SDL_SCANCODE_A && code<=SDL_SCANCODE_Z) return 'A'+code-SDL_SCANCODE_A;
    if (code>=SDL_SCANCODE_1 && code<=SDL_SCANCODE_9) return '1'+code-SDL_SCANCODE_1;
    using namespace gt2::keys;
    switch(code) {
    case SDL_SCANCODE_0:return '0'; case SDL_SCANCODE_RETURN:case SDL_SCANCODE_KP_ENTER:return kReturn;
    case SDL_SCANCODE_BACKSPACE:return kBack; case SDL_SCANCODE_DELETE:return kDelete;
    case SDL_SCANCODE_ESCAPE:return kEscape; case SDL_SCANCODE_SPACE:return kSpace;
    case SDL_SCANCODE_LSHIFT:case SDL_SCANCODE_RSHIFT:return kShift;
    case SDL_SCANCODE_PAGEUP:return kPageUp; case SDL_SCANCODE_PAGEDOWN:return kPageDown;
    case SDL_SCANCODE_HOME:return kHome; case SDL_SCANCODE_LEFT:return kLeft; case SDL_SCANCODE_RIGHT:return kRight;
    case SDL_SCANCODE_UP:return kUp; case SDL_SCANCODE_DOWN:return kDown;
    case SDL_SCANCODE_F5:return kF5; case SDL_SCANCODE_F8:return kF8; case SDL_SCANCODE_F10:return kF10;
    default:return 0;
    }
}
class WebWindow final : public WindowBackend {
public:
    WebWindow(const std::string& title,int w,int h,bool render) {
        if(SDL_InitSubSystem(SDL_INIT_VIDEO)!=0) throw std::runtime_error(SDL_GetError());
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION,3); SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION,0);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,SDL_GL_CONTEXT_PROFILE_ES);
        SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE,24); SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER,1);
        window_=SDL_CreateWindow(title.c_str(),0,0,w,h,SDL_WINDOW_OPENGL|SDL_WINDOW_RESIZABLE);
        if(!window_) throw std::runtime_error(SDL_GetError());
        context_=SDL_GL_CreateContext(window_);
        if(!context_) throw std::runtime_error(SDL_GetError());
        if(render) renderer_=std::make_unique<gt2view::SceneRenderer>();
        emscripten_set_webglcontextlost_callback("#canvas",this,true,[](int,const void*,void* self)->EM_BOOL {
            static_cast<WebWindow*>(self)->lost_=true; return EM_TRUE;
        });
    }
    ~WebWindow() override {
        emscripten_set_webglcontextlost_callback("#canvas",nullptr,true,nullptr);
        renderer_.reset(); SDL_GL_DeleteContext(context_); SDL_DestroyWindow(window_); SDL_QuitSubSystem(SDL_INIT_VIDEO);
    }
    gt2view::SceneRenderer& Renderer() override {
        if(!renderer_) throw std::runtime_error("This window has no renderer"); return *renderer_;
    }
    void* NativeHandle() const override { return window_; }
    void SetTitle(const std::string& title) override { SDL_SetWindowTitle(window_,title.c_str()); }
    void Pump() override {
        // Every game loop (including --fast and menu loops) yields to browser events.
        // Asyncify keeps the original C++ call stack; no second gameplay loop.
        emscripten_sleep(0);
        if(lost_) throw std::runtime_error("Graphics context lost. Export your saves and reload this page.");
        SDL_Event event;
        while(SDL_PollEvent(&event)) {
            if(event.type==SDL_QUIT) closed_=true;
            if(event.type==SDL_CONTROLLERDEVICEADDED || event.type==SDL_CONTROLLERDEVICEREMOVED) devicesChanged_=true;
            if(event.type==SDL_WINDOWEVENT) {
                if(event.window.event==SDL_WINDOWEVENT_FOCUS_LOST || event.window.event==SDL_WINDOWEVENT_FOCUS_GAINED) timingReset_=true;
                if(event.window.event==SDL_WINDOWEVENT_CLOSE) closed_=true;
            }
            if(event.type==SDL_KEYDOWN && !event.key.repeat) {
                if(event.key.keysym.scancode==SDL_SCANCODE_Q && (event.key.keysym.mod&KMOD_SHIFT)) keyDowns_.push_back(gt2::keys::kF10);
                else if(const int key=Key(event.key.keysym.scancode)) keyDowns_.push_back(key);
            }
        }
    }
    void EndRenderFrame() override { SDL_GL_SwapWindow(window_); }
    bool Closed() const override { return closed_; }
    void Close() override { closed_=true; }
    bool Focused() const override { return (SDL_GetWindowFlags(window_)&SDL_WINDOW_INPUT_FOCUS)!=0; }
    bool TakeTimingReset() override { return std::exchange(timingReset_,false); }
    bool TakeDevicesChanged() override { return std::exchange(devicesChanged_,false); }
    std::vector<int> TakeKeyDowns() override { return std::exchange(keyDowns_,{}); }
    bool KeyDown(int key) const override {
        int count; const auto* state=SDL_GetKeyboardState(&count);
        for(int i=0;i<count;++i) if(state[i] && Key(SDL_Scancode(i))==key) return true;
        return false;
    }
    void SleepUntil(Clock::time_point t) override {
        const auto ms=std::chrono::duration_cast<std::chrono::milliseconds>(t-Clock::now()).count();
        if(ms>0) emscripten_sleep(unsigned(std::min<int64_t>(ms,50)));
    }
    bool VBlankTiming(Clock::time_point&,Clock::duration&) const override { return false; }
private:
    SDL_Window* window_=nullptr;
    SDL_GLContext context_=nullptr;
    std::unique_ptr<gt2view::SceneRenderer> renderer_;
    std::vector<int> keyDowns_;
    bool closed_=false,devicesChanged_=false,timingReset_=false,lost_=false;
};
}
std::unique_ptr<WindowBackend> CreateWindowBackend(const std::string& title,int w,int h) { return std::make_unique<WebWindow>(title,w,h,true); }
std::unique_ptr<WindowBackend> CreateInputWindowBackend(const std::string& title,int w,int h) { return std::make_unique<WebWindow>(title,w,h,false); }
std::unique_ptr<WindowBackend> CreateXrWindowBackend(const std::string&,int,int) { throw std::runtime_error("WebGL 2 supports flat play. WebXR is not implemented."); }
}
