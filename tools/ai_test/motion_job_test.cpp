#include "../../src/SL_MotionJob.h"
#include <cstdio>
#include <stdexcept>
#include <thread>

namespace {
std::atomic<int> mode{0},calls{0},active{0};
std::atomic<unsigned> csr{0};
std::thread::id owner;
bool wrongThread=false;
bool cancel(void*) {if(std::this_thread::get_id()!=owner)wrongThread=true;return true;}
void require(bool value,const char* name){if(!value)throw std::runtime_error(name);}
void started(){for(int i=0;i<2000&&!active;i++)std::this_thread::sleep_for(std::chrono::milliseconds(1));require(active==1,"job not started");}
}

// A controlled tracking backend verifies lifecycle and host callback ownership.
bool SL_TrackSurfaceMotion(const float* previous,const float* current,int,int,
    SL_TrackResult& flow,SL_TrackAbort abort,void* data) {
    ++calls;++active;csr=_mm_getcsr();
    struct Leave{~Leave(){--active;}}leave;
    if(mode==2)throw std::bad_alloc();
    if(mode==1){while(!abort(data))std::this_thread::sleep_for(std::chrono::milliseconds(1));return false;}
    flow.depth={*previous+*current};return true;
}
int main(){try{
    owner=std::this_thread::get_id();float a=2,b=3;
    const unsigned original=_mm_getcsr(),changed=original|0x8040;
    _mm_setcsr(changed);
    {SL_MotionJob job;job.start(&a,&b,1,1);auto result=job.finish(nullptr,nullptr);
    require(result&&result->depth[0]==5,"normal result");require(csr==changed,"floating point mode");}
    _mm_setcsr(original);
    mode=1;
    {SL_MotionJob first;first.start(&a,&b,1,1);started();const int before=calls;
     {SL_MotionJob second;second.start(&a,&b,1,1);require(!second.finish(nullptr,nullptr),"busy slot fallback");require(calls==before,"extra worker");}
     require(!first.finish(cancel,nullptr)&&first.interrupted(),"cancel result");}
    require(!active&&!wrongThread,"worker/callback leaked");
    {SL_MotionJob job;job.start(&a,&b,1,1);started();} // Early render error must join before buffers go away.
    require(!active,"destructor did not join");
    mode=2;bool failed=false;
    try{SL_MotionJob job;job.start(&a,&b,1,1);job.finish(nullptr,nullptr);}catch(const std::bad_alloc&){failed=true;}
    require(failed&&!active,"worker exception handling");
    mode=0;
    {SL_MotionJob job;job.start(&a,&b,1,1);require(job.finish(nullptr,nullptr),"slot not released after error");}
    {SL_MotionJob job;job.start(&a,&b,1,1);require(!job.finish(cancel,nullptr)&&job.interrupted(),"ready job cancellation");}
    puts("PASS motion job: result, caller FP mode, bounded worker, host-only callbacks, cancellation, early-error join, exception recovery");return 0;
}catch(const std::exception&e){printf("FAIL %s\n",e.what());return 1;}}
