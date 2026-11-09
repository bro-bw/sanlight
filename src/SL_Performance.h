#pragma once
#include <chrono>
#include <cstdio>
#include <mutex>
#include <Windows.h>
#include <psapi.h>

inline double SL_Milliseconds() {
    return std::chrono::duration<double,std::milli>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

// Numeric timings only. Bounded, process-local diagnostics never affect rendering.
struct SL_Performance {
    double started=SL_Milliseconds(),preMs=0,aiMs=0,surfaceMs=0;
    double checkoutMs=0,convertMs=0,hashMs=0,diskReadMs=0,diskWriteMs=0,shadeMs=0;
    long time=0,scale=0,width=0,height=0,history=0,declarations=0,anchor=-1;
    int state=0,refreshed=0,scanned=0,replayed=0,ai=0,surfaces=0,admitted=-1;
    int entries=0,ownerMatches=0,eligible=0,stateRejected=0;
    int gpu=-1;
    int prefixMemoHits=0,prefixMemoMisses=0;
    int diskHits=0,diskMisses=0,diskWrites=0,diskFailures=0,modelRuns=0;
    const void* owner=nullptr;
    void write(long error) const noexcept {
        try {
            static std::mutex mutex;
            std::lock_guard<std::mutex> lock(mutex);
            wchar_t folder[MAX_PATH]={},path[MAX_PATH]={};
            DWORD length=GetTempPathW(MAX_PATH,folder);
            if(!length||length>=MAX_PATH)return;
            if(swprintf_s(path,MAX_PATH,L"%sSanLight-v1.3-build%d-%lu.log",folder,SL_BUILD_VERSION,GetCurrentProcessId())<0)return;
            HANDLE file=CreateFileW(path,GENERIC_WRITE,FILE_SHARE_READ,nullptr,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
            if(file==INVALID_HANDLE_VALUE)return;
            LARGE_INTEGER size={},zero={};
            if(GetFileSizeEx(file,&size)&&size.QuadPart>2*1024*1024) {
                SetFilePointerEx(file,zero,nullptr,FILE_BEGIN);SetEndOfFile(file);
            }
            SetFilePointerEx(file,zero,nullptr,FILE_END);
            PROCESS_MEMORY_COUNTERS memory={};memory.cb=sizeof(memory);
            auto query=reinterpret_cast<BOOL(WINAPI*)(HANDLE,PPROCESS_MEMORY_COUNTERS,DWORD)>(
                GetProcAddress(GetModuleHandleW(L"kernel32.dll"),"K32GetProcessMemoryInfo"));
            if(query)query(GetCurrentProcess(),&memory,sizeof(memory));
            char line[1400];
            int n=sprintf_s(line,sizeof(line),
                "build=%d owner=%p time=%ld/%ld canvas=%ldx%ld history=%ld declared=%ld state=%d anchor=%ld refreshed=%d scanned=%d replayed=%d ai=%d surface=%d admitted=%d entries=%d owner_matches=%d eligible=%d state_rejected=%d pre_ms=%.1f ai_ms=%.1f surface_ms=%.1f render_ms=%.1f checkout_ms=%.1f convert_ms=%.1f hash_ms=%.1f disk_read_ms=%.1f disk_write_ms=%.1f shade_ms=%.1f disk_hits=%d disk_misses=%d disk_writes=%d disk_failures=%d model_runs=%d gpu=%d prefix_hits=%d prefix_misses=%d process_mb=%.1f process_peak_mb=%.1f error=%ld\r\n",
                SL_BUILD_VERSION,owner,time,scale,width,height,history,declarations,state,anchor,refreshed,scanned,replayed,
                ai,surfaces,admitted,entries,ownerMatches,eligible,stateRejected,preMs,aiMs,surfaceMs,SL_Milliseconds()-started,
                checkoutMs,convertMs,hashMs,diskReadMs,diskWriteMs,shadeMs,diskHits,diskMisses,diskWrites,diskFailures,modelRuns,gpu,prefixMemoHits,prefixMemoMisses,
                memory.WorkingSetSize/1048576.0,memory.PeakWorkingSetSize/1048576.0,error);
            DWORD written=0;if(n>0)WriteFile(file,line,(DWORD)n,&written,nullptr);
            CloseHandle(file);
        } catch(...) {}
    }
};
