#pragma once
#include "game/audio/pause.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

namespace gt2::audio {
// The menu sound driver's 60 Hz clock must keep running while models load.
// Deterministic and recorded runs keep their explicit Frame() calls.
class AudioFrameClock {
public:
    ~AudioFrameClock() { Stop(); }
    bool Running() const { return running_.load(); }
    void Start(std::function<void()> tick) {
        Stop();
#ifndef __EMSCRIPTEN__
        running_=true;
        try {
            thread_=std::thread([this,tick=std::move(tick)] {
                using Clock=std::chrono::steady_clock;
                constexpr auto period=std::chrono::nanoseconds(16'666'667);
                auto next=Clock::now()+period;
                std::unique_lock lock(mutex_);
                while(running_) {
                    if(wake_.wait_until(lock,next,[this]{return !running_;}))break;
                    lock.unlock();
                    if(mixPauseDepth.load()==0)tick();
                    lock.lock();next+=period;
                    if(next+period*6<Clock::now())next=Clock::now()+period;
                }
            });
        }catch(...) {running_=false;throw;}
#else
        (void)tick;
#endif
    }
    void Stop() {
        running_=false;wake_.notify_all();
        if(thread_.joinable())thread_.join();
    }
private:
    std::atomic<bool> running_{false};
    std::mutex mutex_;
    std::condition_variable wake_;
    std::thread thread_;
};
}
