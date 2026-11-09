#pragma once
#include <cstdlib>
#include <map>

namespace SDKHandles {
struct Record { A_HandleSize size; int locks; };
static std::map<PF_Handle, Record> live;
static bool failAllocate=false,failLock=false;
inline PF_Handle allocate(A_HandleSize size) {
    if(failAllocate){failAllocate=false;return nullptr;}
    auto handle=(PF_Handle)std::malloc(sizeof(void*));
    if(!handle)return nullptr;
    *handle=std::calloc(1,size);
    if(!*handle){std::free(handle);return nullptr;}
    live.emplace(handle,Record{size,0});return handle;
}
inline void* lock(PF_Handle handle) {
    if(failLock){failLock=false;return nullptr;}
    auto it=live.find(handle);if(it==live.end())std::abort();
    ++it->second.locks;return *handle;
}
inline void unlock(PF_Handle handle) {
    auto it=live.find(handle);if(it==live.end()||it->second.locks<=0)std::abort();
    --it->second.locks;
}
inline void dispose(PF_Handle handle) {
    auto it=live.find(handle);if(it==live.end()||it->second.locks)std::abort();
    std::free(*handle);std::free(handle);live.erase(it);
}
inline A_HandleSize size(PF_Handle handle) {
    auto it=live.find(handle);if(it==live.end())std::abort();return it->second.size;
}
inline void install(PF_UtilCallbacks& util) {
    util.host_new_handle=allocate;util.host_lock_handle=lock;util.host_unlock_handle=unlock;
    util.host_dispose_handle=dispose;util.host_get_handle_size=size;
}
}
