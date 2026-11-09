#include <cstdio>
#include <cstdlib>
#include <new>
static bool failNext=false;
void* operator new(std::size_t n) {
    if(failNext){failNext=false;throw std::bad_alloc();}
    if(void* p=std::malloc(n?n:1))return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept {std::free(p);}
void operator delete(void* p,std::size_t) noexcept {std::free(p);}
using A_long=long;
using PF_ProgPtr=void*;
struct PF_State {long data[4]={};};
struct PF_LRect {long left=0,top=0,right=0,bottom=0;};
#include "../../src/SL_Checkpoint.h"

int main() {
    SL_CheckpointCache cache(32);SL_HistoryKey key;
    auto make=[&](long time) {
        auto p=std::make_shared<SL_Checkpoint>();p->time=time;p->settings=p->requestSettings=key;
        p->image.resize(2);p->depth=std::make_shared<const std::vector<float>>(2);return p;
    };
    auto same=[](const PF_State&){return true;};
    cache.put(make(0));cache.put(make(1));cache.put(make(1));
    if(!cache.get(nullptr,key,0,1,0,same))return 1; // Replacing a duplicate must not evict time0.
    auto pinned=cache.get(nullptr,key,1,1,0,same);cache.clear();
    if(!pinned||pinned->time!=1||cache.get(nullptr,key,1,1,0,same))return 2;
    auto large=make(0);large->image.resize(100);if(cache.put(large))return 9;
    if(cache.get(nullptr,key,0,1,0,same))return 3;
    auto partial=make(8);partial->minimumTarget=20;cache.put(partial);
    if(cache.get(nullptr,key,10,1,0,same)||!cache.get(nullptr,key,20,1,0,same))return 4;
    if(cache.get(nullptr,key,20,1,0,[](const PF_State&){return false;}))return 5;
    SL_HistoryKey other;other.value(42);
    if(cache.get(nullptr,other,20,1,0,same))return 6;
    cache.clear();auto sub=make(3);cache.put(sub);
    if(!cache.get(nullptr,key,3,2,0,same)||cache.get(nullptr,key,4,2,0,same))return 7;
    cache.clear();auto request=make(0);request->settings=other;cache.put(request);
    if(cache.get(nullptr,key,0,1,0,same)||!cache.get(nullptr,key,0,1,0,same,true))return 8;
    SL_CheckpointLookup probe;
    if(cache.get(nullptr,key,0,1,0,[](const PF_State&){return false;},true,&probe))return 10;
    if(probe.entries!=1||probe.ownerMatches!=1||probe.eligible!=1||probe.stateRejected!=1)return 11;
    probe={};
    if(cache.get(reinterpret_cast<void*>(1),key,0,1,0,same,true,&probe))return 12;
    if(probe.entries!=1||probe.ownerMatches||probe.eligible||probe.stateRejected)return 13;
    SL_RenderSurface rendered;
    rendered.local.assign(8,0.25f);const auto* address=rendered.local.data();
    failNext=true; // control-block allocation fails before ownership transfer
    if(rendered.freeze() || failNext || rendered.pixels().data()!=address || rendered.pixels()[7]!=.25f)return 14;
    const auto shared=rendered.freeze();
    if(!shared || shared->data()!=address || !rendered.local.empty())return 15;

    SL_HistoryCache surfaces(32);
    surfaces.put(key,shared);
    SL_RenderSurface another;another.local.assign(8,.25f);
    if(surfaces.put(key,another.freeze())!=shared)return 16; // concurrent duplicate converges
    failNext=true; // optional insertion fails even though the render buffer is usable
    if(surfaces.put(other,another.shared)!=another.shared || failNext || surfaces.get(other) ||
       surfaces.get(key)!=shared || another.pixels().size()!=8)return 17;
    if(surfaces.put(other,another.shared)!=another.shared || surfaces.get(key)||surfaces.get(other)!=another.shared)return 18;

    SL_CheckpointCache second(40);auto complete=make(0);
    complete->depth=shared;failNext=true;
    if(second.put(complete) || failNext || second.get(nullptr,key,0,1,0,same))return 19;
    if(!second.put(complete))return 20;
    failNext=true;if(second.get(nullptr,key,0,1,0,same) || failNext)return 21;
    if(second.get(nullptr,key,0,1,0,same)!=complete)return 22;
    // Each cache charges all logical bytes even when an owner is shared.
    complete.reset();
    auto replacement=make(1);replacement->depth=another.shared;
    if(!second.put(replacement) || second.get(nullptr,key,0,1,0,same) ||
       second.get(nullptr,key,1,1,0,same)!=replacement)return 23;
    surfaces.clear();second.clear();if(shared->data()!=address || (*shared)[7]!=.25f)return 24;
    SL_CheckpointCache rotating(1024);
    const SL_InstanceID firstID{11,22},secondID{33,44};
    auto first=make(0);first->owner=reinterpret_cast<void*>(1);first->instance=firstID;
    if(!rotating.put(first))return 25;
    auto find=[&](void* owner,const SL_InstanceID& id,bool state=true,const SL_HistoryKey& config=SL_HistoryKey{}) {
        return rotating.get(owner,config,0,1,0,[&](const PF_State&){return state;},false,nullptr,id);
    };
    if(find(reinterpret_cast<void*>(2),firstID)!=first || find(first->owner,secondID) ||
       find(first->owner,{}) || find(first->owner,firstID,false) || find(first->owner,firstID,true,other))return 26;
    auto newer=make(0);newer->owner=reinterpret_cast<void*>(3);newer->instance=firstID;
    rotating.put(newer);
    if(find(first->owner,firstID)!=newer)return 27; // replace across temporary host references
    auto separate=make(0);separate->owner=first->owner;separate->instance=secondID;rotating.put(separate);
    if(find(first->owner,firstID)!=newer || find(first->owner,secondID)!=separate)return 28;
    auto legacy=make(0);legacy->owner=first->owner;rotating.put(legacy);
    if(find(first->owner,{})!=legacy || find(reinterpret_cast<void*>(2),{}) ||
       find(first->owner,firstID)!=newer || find(first->owner,secondID)!=separate)return 29;
    puts("PASS rotating host references reuse one sequence; duplicate instances, states, settings and unknown identities stay isolated");
    puts("PASS shared allocation identity, duplicate publication, independent budgets, pinned lifetime and allocation failures");
    puts("PASS checkpoint budget, duplicate replacement, pinned ownership, state/settings isolation, subframes and target horizon");
}
