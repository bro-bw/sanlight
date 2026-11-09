#include "AEConfig.h"
#include "AE_Effect.h"
#include "AE_EffectCB.h"
#include "AE_GeneralPlug.h"
#include "SPBasic.h"
#include "sdk_handle_fixture.h"
#include "../../src/SL_Sequence.h"
#include <cstdio>
#include <set>
#include <thread>
#include <mutex>

#define REQUIRE(x) do{if(!(x)){printf("FAIL line %d: %s\n",__LINE__,#x);return 1;}}while(0)
static int suites=0;
static bool available=true,readError=false;
static PF_Handle constHandle=nullptr;
static PF_Err getConst(PF_ProgPtr,PF_ConstHandle* handle) {
    *handle=readError?nullptr:(PF_ConstHandle)constHandle;return readError?13:0;
}
static PF_EffectSequenceDataSuite1 sequenceSuite{getConst};
static SPErr acquire(const char* name,int32 version,const void** result) {
    *result=nullptr;
    if(!available||strcmp(name,kPFEffectSequenceDataSuite)||version!=kPFEffectSequenceDataSuiteVersion1)return 13;
    *result=&sequenceSuite;++suites;return 0;
}
static SPErr release(const char*,int32){--suites;return 0;}

int main() {
    PF_UtilCallbacks util{};SDKHandles::install(util);
    SPBasicSuite basic{};basic.AcquireSuite=acquire;basic.ReleaseSuite=release;
    PF_InData in{};in.utils=&util;in.pica_basicP=&basic;
    PF_OutData out{};
    REQUIRE(!SL_SequenceCommand(PF_Cmd_SEQUENCE_SETUP,&in,&out));
    in.sequence_data=out.sequence_data;const auto original=SL_ReadInstanceID(&in);
    REQUIRE(original.valid());
    REQUIRE(!SL_SequenceCommand(PF_Cmd_GET_FLATTENED_SEQUENCE_DATA,&in,&out));
    PF_Handle flat=out.sequence_data;
    REQUIRE(flat!=in.sequence_data && SDKHandles::live.size()==2);
    REQUIRE(SL_ReadInstanceID(&in)==original);

    PF_InData render=in;render.sequence_data=flat;render.in_flags=PF_InFlag_PROJECT_IS_RENDER_ONLY;
    REQUIRE(!SL_SequenceCommand(PF_Cmd_SEQUENCE_RESETUP,&render,&out));
    render.sequence_data=out.sequence_data;
    REQUIRE(SL_ReadInstanceID(&render)==original && SDKHandles::live.size()==2);
    REQUIRE(!SL_SequenceCommand(PF_Cmd_SEQUENCE_FLATTEN,&render,&out));
    REQUIRE(out.sequence_data==render.sequence_data);
    REQUIRE(!SL_SequenceCommand(PF_Cmd_SEQUENCE_SETDOWN,&render,&out));
    REQUIRE(!out.sequence_data && SDKHandles::live.size()==1);

    // Read-only render data supplied exclusively by the const suite.
    constHandle=in.sequence_data;render.sequence_data=nullptr;
    REQUIRE(SL_ReadInstanceID(&render)==original && !suites);
    available=false;REQUIRE(!SL_ReadInstanceID(&render).valid() && !suites);available=true;
    readError=true;REQUIRE(!SL_ReadInstanceID(&render).valid() && !suites);readError=false;
    constHandle=SDKHandles::allocate(1);
    REQUIRE(!SL_ReadInstanceID(&render).valid() && !suites);
    SDKHandles::dispose(constHandle);constHandle=nullptr;

    // Independent UI copies and the original after duplicate/load each get a new ID.
    REQUIRE(!SL_SequenceCommand(PF_Cmd_GET_FLATTENED_SEQUENCE_DATA,&in,&out));
    PF_InData duplicate=in;duplicate.sequence_data=out.sequence_data;
    REQUIRE(!SL_SequenceCommand(PF_Cmd_SEQUENCE_RESETUP,&duplicate,&out));
    duplicate.sequence_data=out.sequence_data;auto copied=SL_ReadInstanceID(&duplicate);
    REQUIRE(copied.valid() && !(copied==original));
    REQUIRE(!SL_SequenceCommand(PF_Cmd_SEQUENCE_RESETUP,&in,&out));
    in.sequence_data=out.sequence_data;auto reset=SL_ReadInstanceID(&in);
    REQUIRE(reset.valid() && !(reset==original) && !(reset==copied));

    // A failed resetup cannot destroy the live instance or leak its replacement.
    auto saved=in.sequence_data;out.sequence_data=saved;
    SDKHandles::failAllocate=true;
    REQUIRE(SL_SequenceCommand(PF_Cmd_SEQUENCE_RESETUP,&in,&out)==PF_Err_OUT_OF_MEMORY);
    REQUIRE(out.sequence_data==saved && SL_ReadInstanceID(&in)==reset && SDKHandles::live.size()==2);
    // Exercise write-lock failure separately from read-lock failure.
    auto data=SL_NewSequence();SDKHandles::failLock=true;
    REQUIRE(SL_WriteSequence(&in,&out,data,true)==PF_Err_OUT_OF_MEMORY);
    REQUIRE(out.sequence_data==saved && SL_ReadInstanceID(&in)==reset && SDKHandles::live.size()==2);

    PF_InData corrupt=in;corrupt.sequence_data=SDKHandles::allocate(3);
    REQUIRE(!SL_ReadInstanceID(&corrupt).valid());
    corrupt.in_flags=PF_InFlag_PROJECT_IS_RENDER_ONLY;
    REQUIRE(!SL_SequenceCommand(PF_Cmd_SEQUENCE_RESETUP,&corrupt,&out));
    corrupt.sequence_data=out.sequence_data;REQUIRE(SL_ReadInstanceID(&corrupt).valid());
    REQUIRE(!SL_SequenceCommand(PF_Cmd_SEQUENCE_SETDOWN,&corrupt,&out));
    REQUIRE(!SL_SequenceCommand(PF_Cmd_SEQUENCE_SETDOWN,&duplicate,&out));
    REQUIRE(!SL_SequenceCommand(PF_Cmd_SEQUENCE_SETDOWN,&in,&out));
    in.sequence_data=nullptr;
    REQUIRE(!SL_SequenceCommand(PF_Cmd_GET_FLATTENED_SEQUENCE_DATA,&in,&out) && !out.sequence_data);
    REQUIRE(!SL_SequenceCommand(PF_Cmd_SEQUENCE_SETDOWN,&in,&out));
    REQUIRE(SDKHandles::live.empty() && !suites);

    std::set<std::pair<std::uint64_t,std::uint64_t>> ids;std::mutex mutex;
    std::thread workers[4];bool valid=true;
    for(auto& worker:workers)worker=std::thread([&]{for(int i=0;i<1000;++i){
        auto id=SL_NewSequence().instance;std::lock_guard<std::mutex> guard(mutex);
        valid=valid&&id.valid()&&ids.emplace(id.lo,id.hi).second;
    }});
    for(auto& worker:workers)worker.join();
    REQUIRE(valid && ids.size()==4000);
    puts("PASS sequence clone persistence, UI copy isolation, flat ownership, const-suite/failure fallback, allocation failure and concurrent IDs");
}
