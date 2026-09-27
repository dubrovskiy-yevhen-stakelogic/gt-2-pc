// Runs without game assets, a display or physical controllers. Real Mac acceptance is separate.
#define SDL_MAIN_HANDLED
#include <SDL.h>
#include "platform/input/input_system.h"
#include "game/audio/audio_device.h"
#include <atomic>
#include <cstdio>
#include <stdexcept>

namespace {
void Require(bool value, const char* reason) { if (!value) throw std::runtime_error(reason); }
struct AudioProbe final : gt2::audio::StreamSource {
    std::atomic<size_t> frames{0};
    void MixStream(float* output, size_t count) override {
        for (size_t i = 0; i < count * 2; ++i) output[i] += 0.125f;
        frames += count;
    }
};
}
int main() {
    try {
        SDL_SetMainReady();
        SDL_setenv("SDL_AUDIODRIVER", "dummy", 1);
        SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
        Require(SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) == 0, SDL_GetError());
        const int index = SDL_JoystickAttachVirtual(SDL_JOYSTICK_TYPE_GAMECONTROLLER, SDL_CONTROLLER_AXIS_MAX, SDL_CONTROLLER_BUTTON_MAX, 0);
        Require(index >= 0, SDL_GetError());
        auto* joystick = SDL_JoystickOpen(index);
        Require(joystick != nullptr, SDL_GetError());
        // Virtual trigger axes use the joystick's full signed range, with -32768 at rest.
        SDL_JoystickSetVirtualAxis(joystick, SDL_CONTROLLER_AXIS_TRIGGERLEFT, -32768);
        SDL_JoystickSetVirtualAxis(joystick, SDL_CONTROLLER_AXIS_TRIGGERRIGHT, -32768);
        {
            gt2::input::InputSystem input;
            input.Poll(0, true);
            Require(input.Port1().type == gt2::input::kTypeAnalog, "virtual gamepad not discovered");
            SDL_JoystickSetVirtualButton(joystick, SDL_CONTROLLER_BUTTON_A, 1);
            SDL_JoystickSetVirtualAxis(joystick, SDL_CONTROLLER_AXIS_LEFTX, 32767);
            SDL_JoystickSetVirtualAxis(joystick, SDL_CONTROLLER_AXIS_TRIGGERRIGHT, 32767);
            input.Poll(1, true);
            Require((input.Port1().buttons & gt2::input::ps1::kCross) != 0, "Cross mapping");
            Require(input.Port1().analog[2] == 255 && input.Port1().pressureR2 == 255, "stick / pedal mapping");
            input.Poll(2, false);
            Require(input.Port1().buttons == 0 && input.Port1().analog[2] == 128 && input.Port1().pressureR2 == 0, "focus loss must neutralize controls");
            input.Poll(3, true);
            Require((input.Port1().buttons & gt2::input::ps1::kCross) != 0, "focus regain");
            SDL_JoystickClose(joystick);
            Require(SDL_JoystickDetachVirtual(index) == 0, SDL_GetError());
            input.Poll(4, true);
            Require(input.Port1().type == gt2::input::kTypeNone, "disconnect must clear port");
            input.AddFakePad("0:cross=1:2,0:lx=255", 1);
            input.AddFakePad("0:square=1", 2);
            input.Poll(0, false);
            Require((input.Port1().buttons & gt2::input::ps1::kCross) != 0 && input.Port1().analog[2] == 255, "scripted first port");
            Require((input.Port2().buttons & gt2::input::ps1::kSquare) != 0, "scripted second port");
            input.Poll(2, false);
            Require(input.Port1().buttons == 0, "script hold expiry");
        }
        SDL_QuitSubSystem(SDL_INIT_GAMECONTROLLER);
        gt2::audio::Mixer mixer;
        AudioProbe probe;
        mixer.SetStream(&probe);
        gt2::audio::AudioDevice audio;
        std::string error;
        if (!audio.Open(mixer, error)) throw std::runtime_error(error);
        const auto start = SDL_GetTicks64();
        while (probe.frames < 4410 && SDL_GetTicks64() - start < 2000) SDL_Delay(5);
        Require(probe.frames >= 4410, "audio device must keep feeding the mixer");
        audio.Close();
        const auto stopped = probe.frames.load();
        SDL_Delay(30);
        Require(probe.frames == stopped && !audio.IsOpen(), "audio close must join its worker");
        if (!audio.Open(mixer, error)) throw std::runtime_error(error);
        audio.Close();
        mixer.SetStream(nullptr);
        std::puts("SDL checks passed: gamepad axes/buttons, focus, disconnect, two scripted ports, audio streaming/reopen.");
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "SDL check failed: %s\n", e.what());
        return 1;
    }
}
