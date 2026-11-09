#pragma once
#include "SL_History.h"
#include "SL_InstanceID.h"
#include <memory>
#include <climits>

struct SL_Checkpoint {
    PF_ProgPtr owner=nullptr;
    SL_InstanceID instance={};
    PF_State state={};
    SL_HistoryKey settings,requestSettings,prefix;
    PF_LRect canvas={};
    A_long time=0;
    A_long minimumTarget=LONG_MIN;
    std::vector<float> image;
    SL_SurfaceRef depth;
};

struct SL_CheckpointLookup {
    int entries=0,ownerMatches=0,eligible=0,stateRejected=0;
};

class SL_CheckpointCache {
    std::mutex mutex;
    std::vector<std::shared_ptr<const SL_Checkpoint>> entries;
    size_t bytes=0,limit;
    static size_t size(const SL_Checkpoint& entry) {
        return (entry.image.size()+entry.depth->size())*sizeof(float);
    }
public:
    explicit SL_CheckpointCache(size_t memoryLimit=512*size_t(1024)*1024):limit(memoryLimit) {}
    template<class SameState>
    std::shared_ptr<const SL_Checkpoint> get(PF_ProgPtr owner,const SL_HistoryKey& settings,
        A_long time,long long step,long long start,SameState same,bool requestOnly=false,
        SL_CheckpointLookup* probe=nullptr,const SL_InstanceID& instance={}) {
        std::vector<std::shared_ptr<const SL_Checkpoint>> candidates;
        try {std::lock_guard<std::mutex> guard(mutex);candidates=entries;}
        catch(const std::bad_alloc&) { return {}; }
        std::shared_ptr<const SL_Checkpoint> best;
        // Never invoke a host callback while holding the cache mutex.
        for(const auto& p:candidates) {
            if(probe)++probe->entries;
            if(!SL_SameCheckpointOwner(p->owner,p->instance,owner,instance))continue;
            if(probe)++probe->ownerMatches;
            const auto& candidate=requestOnly?p->requestSettings:p->settings;
            if(candidate<settings || settings<candidate || p->time>time || p->time<start || time<p->minimumTarget)continue;
            // A subframe is a terminal sample, never an anchor for a different frame.
            if(step>0 ? (p->time!=time && p->time%step!=0) : p->time!=time)continue;
            if(probe)++probe->eligible;
            if(best && best->time>=p->time)continue;
            if(same(p->state))best=p;
            else if(probe)++probe->stateRejected;
        }
        return best;
    }
    bool put(std::shared_ptr<const SL_Checkpoint> entry) {
        // A 4K RGBA + joint surface checkpoint exceeds the previous 192 MiB cap.
        if (!entry || !entry->depth) return false;
        const size_t amount=size(*entry);
        if(amount>limit)return false;
        std::lock_guard<std::mutex> guard(mutex);
        // Secure the new slot before replacing or evicting existing entries.
        try { entries.reserve(12); }
        catch(const std::bad_alloc&) { return false; }
        for(auto it=entries.begin();it!=entries.end();) {
            const auto& old=**it;
            if(SL_SameCheckpointOwner(old.owner,old.instance,entry->owner,entry->instance)&&
               old.time==entry->time&&!(old.settings<entry->settings)&&!(entry->settings<old.settings)) {
                bytes-=size(old);it=entries.erase(it);
            } else ++it;
        }
        while(!entries.empty()&&(bytes+amount>limit||entries.size()>=12)) {
            bytes-=size(*entries.front());entries.erase(entries.begin());
        }
        entries.push_back(std::move(entry));bytes+=amount;return true;
    }
    void clear() {std::lock_guard<std::mutex> guard(mutex);entries.clear();bytes=0;}
};
