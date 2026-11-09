#pragma once
#include "SL_Surface.h"
#include <atomic>
#include <chrono>
#include <future>
#include <mutex>
#include <xmmintrin.h>

// One extra motion job per process, even when AE renders many frames at once.
// AE callbacks run only on the caller. The borrowed images outlive this job.
class SL_MotionJob {
    std::unique_lock<std::mutex> slot;
    SL_TrackResult flow;
    std::atomic<bool> cancelled{false};
    std::future<bool> job;
public:
    void start(const float* previous,const float* current,int w,int h) {
        static std::mutex gate;
        slot=std::unique_lock<std::mutex>(gate,std::try_to_lock);
        if(!slot.owns_lock())return;
        unsigned mode=_mm_getcsr();
        try {
            job=std::async(std::launch::async,[=] {
                unsigned saved=_mm_getcsr();
                struct Restore { unsigned value; ~Restore(){_mm_setcsr(value);} } restore{saved};
                _mm_setcsr(mode);
                return SL_TrackSurfaceMotion(previous,current,w,h,flow,[](void* data) {
                    return static_cast<SL_MotionJob*>(data)->cancelled.load(std::memory_order_relaxed);
                },this);
            });
        } catch(const std::system_error&) {} catch(const std::bad_alloc&) {}
    }
    SL_TrackResult* finish(SL_TrackAbort abort,void* data) {
        if(!job.valid())return nullptr;
        while(job.wait_for(std::chrono::milliseconds(8))!=std::future_status::ready)
            if(abort&&abort(data))cancelled.store(true,std::memory_order_relaxed);
        if(abort&&abort(data))cancelled.store(true,std::memory_order_relaxed);
        const bool complete=job.get();
        return complete&&!interrupted()?&flow:nullptr;
    }
    bool interrupted() const {return cancelled.load(std::memory_order_relaxed);}
    ~SL_MotionJob(){
        cancelled.store(true,std::memory_order_relaxed);
        if(job.valid())job.wait();
    }
};
