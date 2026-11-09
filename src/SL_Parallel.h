#pragma once
#include <algorithm>
#include <atomic>
#include <exception>
#include <mutex>
#include <thread>
#include <vector>

// Independent rows only. Host callbacks stay on the calling render thread.
template<class Row>
bool SL_ParallelRows(int rows,Row row,bool (*abort)(void*)=nullptr,void* data=nullptr) {
    if(abort&&abort(data))return false;
    const unsigned workers=rows<32?1:(std::min)(4u,(std::max)(1u,std::thread::hardware_concurrency()));
    if(workers==1) {
        for(int y=0;y<rows;y++) {if(abort&&abort(data))return false;row(y);}
        return true;
    }
    std::atomic<int> next{0};std::atomic<bool> stopped{false};
    std::exception_ptr failure;std::mutex failureMutex;
    auto work=[&](bool caller) {
        try {
            while(!stopped.load(std::memory_order_relaxed)) {
                if(caller&&abort&&abort(data)){stopped=true;break;}
                int y=next.fetch_add(1,std::memory_order_relaxed);if(y>=rows)break;
                row(y);
            }
        } catch(...) {
            std::lock_guard<std::mutex> lock(failureMutex);
            if(!failure)failure=std::current_exception();stopped=true;
        }
    };
    std::vector<std::thread> threads;
    struct Join {std::vector<std::thread>& threads;~Join(){for(auto& t:threads)if(t.joinable())t.join();}} join{threads};
    threads.reserve(workers-1);
    for(unsigned i=1;i<workers;i++) {
        try {threads.emplace_back(work,false);}
        catch(const std::system_error&) {break;} // The caller can finish if a worker cannot start.
    }
    work(true);
    for(auto& t:threads)t.join();
    if(failure)std::rethrow_exception(failure);
    return !stopped.load()&&!(abort&&abort(data));
}
